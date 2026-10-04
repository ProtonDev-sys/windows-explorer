#include "explorer/app.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/search.hpp"
#include "explorer/selection.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/search_presentation_store.hpp"
#include "explorer/extra_operations.hpp"
#include "explorer/input.hpp"
#include "explorer/item_actions.hpp"
#include "explorer/status.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/theme.hpp"
#include "explorer/chrome.hpp"
#include "explorer/worker_sta.hpp"
#include "explorer/typed_address.hpp"
#include <initguid.h>
#include <oleacc.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <winnetwk.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <sstream>

namespace explorer {
namespace {
constexpr wchar_t WindowClass[] = L"WindowsExplorer.Native.Window";
constexpr UINT DeferredUpdate = WM_APP + 1;
constexpr UINT DeferredView = WM_APP + 2;
constexpr UINT ShareResult = WM_APP + 3;
constexpr UINT NamespaceResult = WM_APP + 4;
constexpr UINT TypedAddressFirst = 44000;
std::filesystem::path quickAccessSettingsPath() {
    const auto settings = preferencesPath();
    return settings.empty() ? std::filesystem::path{} : settings.parent_path() / L"qat.ini";
}
constexpr std::array<const wchar_t*, 8> ViewNames{
    L"Extra large icons", L"Large icons", L"Medium icons", L"Small icons",
    L"List", L"Details", L"Tiles", L"Content"};
bool itemHasFastType(IShellItem* item,const wchar_t* expected) {
    ComPtr<IShellItem2> extended;ComPtr<IPropertyStore> properties;PROPVARIANT value{};
    bool matches=false;
    if(item&&SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&extended)))&&
       SUCCEEDED(extended->GetPropertyStore(GPS_FASTPROPERTIESONLY|GPS_BESTEFFORT,IID_PPV_ARGS(&properties)))&&
       SUCCEEDED(properties->GetValue(PKEY_ItemType,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)
        matches=_wcsicmp(value.pwszVal,expected)==0;
    PropVariantClear(&value);return matches;
}
bool isExternalSearch(PCIDLIST_ABSOLUTE pidl) {
    PWSTR raw = nullptr;
    if (!pidl || FAILED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &raw))) return false;
    const std::wstring name(raw); CoTaskMemFree(raw);
    if (_wcsnicmp(name.c_str(), L"search-ms:", 10) == 0) return true;
    if (name.size() < 10 || _wcsicmp(name.c_str() + name.size() - 10, L".search-ms") != 0) return false;
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item)))) return false;
    return itemHasFastType(item.Get(),L".search-ms");
}
bool hasNativeItemType(PCIDLIST_ABSOLUTE pidl, const wchar_t* type) {
    ComPtr<IShellItem2> item;
    if (!pidl || FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item)))) return false;
    return itemHasFastType(item.Get(),type);
}

std::wstring textOf(HWND window) {
    const auto length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(length);
    return value;
}
std::wstring itemName(IShellItem* item, SIGDN format) {
    PWSTR value = nullptr;
    if (!item || FAILED(item->GetDisplayName(format, &value))) return {};
    std::wstring result = value;
    CoTaskMemFree(value);
    return result;
}
bool isPhysicalDirectory(IShellItem* item) {
    const auto path = itemName(item, SIGDN_FILESYSPATH);
    if (path.empty()) return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring pidlName(PCIDLIST_ABSOLUTE pidl, SIGDN format) {
    PWSTR value = nullptr;
    if (FAILED(SHGetNameFromIDList(pidl, format, &value))) return {};
    std::wstring result = value;
    CoTaskMemFree(value);
    return result;
}
std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned backslashes = 0;
    for (auto ch : value) {
        if (ch == L'\\') { ++backslashes; continue; }
        result.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        result += ch;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}
HRESULT unusedPath(const std::filesystem::path& parent, const std::wstring& stem, const std::wstring& extension, std::filesystem::path& out) {
    std::error_code error;
    for (unsigned i = 1; i <= 10000; ++i) {
        auto name = stem + (i == 1 ? L"" : L" (" + std::to_wstring(i) + L")") + extension;
        if (!validLeafName(name)) return E_INVALIDARG;
        auto candidate = parent / name;
        const bool exists = std::filesystem::exists(candidate, error);
        if (error) return HRESULT_FROM_WIN32(error.value());
        if (!exists) { out = std::move(candidate); return S_OK; }
    }
    return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
}
HRESULT shellExecute(HWND owner, const wchar_t* target, const wchar_t* arguments = nullptr, const wchar_t* directory = nullptr) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner; info.lpVerb = L"open"; info.lpFile = target;
    info.lpParameters = arguments; info.lpDirectory = directory; info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
}

ExplorerApp::ExplorerApp(HINSTANCE instance, bool headless, RibbonLayout ribbonLayout)
    : instance_(instance), headless_(headless), preferences_(headless ? Preferences{} : loadPreferences(preferencesPath())),
      quickAccessModel_(headless ? QuickAccessToolbar{} : loadQuickAccessToolbar(quickAccessSettingsPath())) {
    requestedRibbonLayout_=ribbonLayout;
    expandCurrent_ = preferences_.expandToCurrent;
    showAllFolders_ = preferences_.showAllFolders;
    showLibraries_ = preferences_.showLibraries;
    if (!headless_) {
        SHELLSTATE settings{};
        SHGetSetSettings(&settings, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN | SSF_SHOWEXTENSIONS, FALSE);
        preferences_.showHidden = settings.fShowAllObjects;
        preferences_.showExtensions = settings.fShowExtensions;
        showSuperHidden_ = settings.fShowSuperHidden;
        refreshCabinetPolicy();
        searchSuggestionsAllowed_ = searchSuggestionsAllowed();
        if (searchSuggestionsAllowed_) loadSearchHistory(searchHistoryPath(), &recentSearches_);
        refreshAddressHistoryPolicy();
    }
}

ExplorerApp::~ExplorerApp() {
    nativeShare_.reset();
    if(searchAutocomplete_)searchAutocomplete_->Enable(FALSE);
    searchAutocomplete_.Reset();searchSuggestions_.Reset();
    namespaceActions_.reset(true);
    backgroundActions_.reset(true);
    archiveActions_.reset(true);
    forgetRibbonTheme(ribbon_.framework());
    ribbon_.reset();
    destroyBrowser();
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    namespaceActions_.reset(true);
    forgetRibbonTheme(ribbon_.framework());
    ribbon_.reset();
    if (folderIcon_) DestroyIcon(folderIcon_);
    if (largeIcon_) DestroyIcon(largeIcon_);
    if (breadcrumbImages_) ImageList_Destroy(breadcrumbImages_);
    if (font_) DeleteObject(font_);
}

HRESULT ExplorerApp::QueryInterface(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IExplorerBrowserEvents) *out = static_cast<IExplorerBrowserEvents*>(this);
    else if (iid == IID_IServiceProvider) *out = static_cast<IServiceProvider*>(this);
    else if (iid == IID_IExplorerPaneVisibility) *out = static_cast<IExplorerPaneVisibility*>(this);
    else if (iid == IID_ICommDlgBrowser || iid == IID_ICommDlgBrowser2 || iid == IID_ICommDlgBrowser3) *out = static_cast<ICommDlgBrowser3*>(this);
    else if (iid == IID_IFolderFilter) *out = static_cast<IFolderFilter*>(this);
    if (!*out) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
ULONG ExplorerApp::AddRef() { return ++references_; }
ULONG ExplorerApp::Release() { const auto count = --references_; if (!count) delete this; return count; }
HRESULT ExplorerApp::QueryService(REFGUID service, REFIID iid, void** out) {
    if (service == SID_ExplorerPaneVisibility || service == SID_SExplorerBrowserFrame) return QueryInterface(iid, out);
    if (!out) return E_POINTER;
    *out = nullptr; return E_NOINTERFACE;
}
HRESULT ExplorerApp::GetPaneState(REFEXPLORERPANE pane, EXPLORERPANESTATE* state) {
    if (!state) return E_POINTER;
    bool visible = false;
    if (pane == EP_NavPane) visible = preferences_.navigationPane;
    else if (pane == EP_PreviewPane) visible = preferences_.previewPane;
    else if (pane == EP_DetailsPane) visible = preferences_.detailsPane;
    // The host owns its ribbon, search box, and status bar.
    *state = static_cast<EXPLORERPANESTATE>((visible ? EPS_DEFAULT_ON : EPS_DEFAULT_OFF) | EPS_FORCE);
    return S_OK;
}
HRESULT ExplorerApp::GetViewFlags(DWORD* flags) {
    if (!flags) return E_POINTER;
    *flags = CDB2GVF_NOSELECTVERB | CDB2GVF_ALLOWPREVIEWPANE;
    // Native searches must enumerate in the background. Regular folders use
    // our attribute filter to hide hidden files independently of Explorer.exe.
    if (navigating_ ? pendingSearchBackground_ : searchBackground_) *flags |= CDB2GVF_NOINCLUDEITEM;
    if (preferences_.showHidden) *flags |= CDB2GVF_SHOWALLFILES;
    return S_OK;
}
HRESULT ExplorerApp::ShouldShow(IShellFolder* folder, PCIDLIST_ABSOLUTE parent, PCUITEMID_CHILD item) {
    if (!folder || !item) return S_OK;
    // This filter owns only the current/pending content folder. The native
    // navigation tree retains its own enumeration rules for other ancestors.
    if(parent && (!currentPidl_||!ILIsEqual(parent,currentPidl_.get())) && (!pendingPidl_||!ILIsEqual(parent,pendingPidl_.get())))return S_OK;
    SFGAOF attributes = SFGAO_HIDDEN | SFGAO_SYSTEM;
    if (FAILED(folder->GetAttributesOf(1, &item, &attributes))) return S_OK;
    if (!showSuperHidden_ && (attributes & (SFGAO_HIDDEN | SFGAO_SYSTEM)) == (SFGAO_HIDDEN | SFGAO_SYSTEM)) return S_FALSE;
    return !preferences_.showHidden && (attributes & SFGAO_HIDDEN) ? S_FALSE : S_OK;
}
HRESULT ExplorerApp::GetEnumFlags(IShellFolder*, PCIDLIST_ABSOLUTE parent, HWND* owner, DWORD* flags) {
    if (!flags) return E_POINTER;
    if(parent && (!currentPidl_||!ILIsEqual(parent,currentPidl_.get())) && (!pendingPidl_||!ILIsEqual(parent,pendingPidl_.get())))return S_OK;
    if (owner) *owner = window_;
    if (preferences_.showHidden) { *flags |= SHCONTF_INCLUDEHIDDEN;
        if(showSuperHidden_) *flags |= SHCONTF_INCLUDESUPERHIDDEN;else *flags &= ~SHCONTF_INCLUDESUPERHIDDEN;
    }
    else *flags &= ~(SHCONTF_INCLUDEHIDDEN | SHCONTF_INCLUDESUPERHIDDEN);
    return S_OK;
}
HRESULT ExplorerApp::GetDefaultMenuText(IShellView*, LPWSTR text, int size) {
    if (text && size > 0) text[0] = 0;
    return E_NOTIMPL;
}
HRESULT ExplorerApp::GetCurrentFilter(LPWSTR text, int size) {
    if (!text || size <= 0) return E_INVALIDARG;
    text[0] = 0;
    return S_OK;
}

void ExplorerApp::updateCaptionIcon() {
    std::array<wchar_t,32768> windows{};
    const auto length=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));
    if(!length||length>=windows.size())return;
    const auto executable=(std::filesystem::path(windows.data())/L"explorer.exe").wstring();
    HICON iconLarge=nullptr,iconSmall=nullptr;
    if(FAILED(SHDefExtractIconW(executable.c_str(),0,0,&iconLarge,&iconSmall,MAKELONG(px(32),px(16)))))return;
    if(iconSmall) {SendMessageW(window_,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(iconSmall));if(folderIcon_)DestroyIcon(folderIcon_);folderIcon_=iconSmall;}
    if(iconLarge) {SendMessageW(window_,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(iconLarge));if(largeIcon_)DestroyIcon(largeIcon_);largeIcon_=iconLarge;}
}

HRESULT ExplorerApp::refreshCabinetPolicy() {
    if(headless_)return E_ACCESSDENIED;
    CABINETSTATE settings{};
    // FALSE means Windows supplied its documented defaults, not an error.
    cabinetPolicyStatus_=ReadCabinetState(&settings,sizeof(settings))?S_OK:S_FALSE;
    fullPathTitle_=settings.fFullPathTitle;
    newWindowMode_=settings.fNewWindowMode;
    saveLocalView_=settings.fSaveLocalView;
    return cabinetPolicyStatus_;
}

void ExplorerApp::updateFrameTitle() {
    if(!window_)return;
    std::wstring title=quickAccessLocation_?L"File Explorer":currentName_;
    if(fullPathTitle_&&filesystemFolder_&&currentPidl_) {
        const auto path=pidlName(currentPidl_.get(),SIGDN_FILESYSPATH);
        if(!path.empty())title=path;
    }
    SetWindowTextW(window_,title.c_str());
}

HRESULT ExplorerApp::openNewWindow(const std::wstring& location) {
    if(headless_)return E_ACCESSDENIED;
    if(location.empty())return E_INVALIDARG;
    std::wstring executable(32768,L'\0');
    const auto count=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(!count)return HRESULT_FROM_WIN32(GetLastError());
    if(count>=executable.size())return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    executable.resize(count);
    return shellExecute(window_,executable.c_str(),(L"--path "+quoteArgument(location)).c_str());
}

HRESULT ExplorerApp::OnDefaultCommand(IShellView* source) {
    if(!newWindowMode_||!source)return S_FALSE;
    if(headless_)return E_ACCESSDENIED;
    if(navigating_||closing_||!view_)return S_FALSE;
    // Preserve the view's modifier-specific default command (for example Alt
    // properties); this policy applies to an ordinary folder activation.
    if((GetKeyState(VK_CONTROL)|GetKeyState(VK_SHIFT)|GetKeyState(VK_MENU))&0x8000)return S_FALSE;
    ComPtr<IUnknown> activeIdentity,sourceIdentity;
    if(FAILED(view_.As(&activeIdentity))||FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceIdentity)))||
       activeIdentity.Get()!=sourceIdentity.Get())return S_FALSE;
    ComPtr<IShellItemArray> selected;
    DWORD count=0;
    if(FAILED(selection(selected))||!selected||FAILED(selected->GetCount(&count))||count!=1)return S_FALSE;
    ComPtr<IShellItem> item;SFGAOF attributes=0;
    if(FAILED(selected->GetItemAt(0,&item))||FAILED(item->GetAttributes(SFGAO_FOLDER,&attributes))||
       !(attributes&SFGAO_FOLDER))return S_FALSE;
    const auto location=itemName(item.Get(),SIGDN_DESKTOPABSOLUTEPARSING);
    const auto hr=openNewWindow(location);
    return SUCCEEDED(hr)?S_OK:hr;
}

HRESULT ExplorerApp::create(const std::wstring& location) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance_; wc.lpszClassName = WindowClass; wc.lpfnWndProc = windowProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"Windows Explorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, preferences_.windowWidth, preferences_.windowHeight,
                             nullptr, nullptr, instance_, this);
    if (!window_) return HRESULT_FROM_WIN32(GetLastError());
    applyWindowTheme(window_);
    dpi_ = GetDpiForWindow(window_);
    updateCaptionIcon();
    RECT initialBounds{}; GetWindowRect(window_, &initialBounds);
    SetWindowPos(window_, nullptr, 0, 0, std::max(px(600), static_cast<int>(initialBounds.right - initialBounds.left)),
                 std::max(px(480), static_cast<int>(initialBounds.bottom - initialBounds.top)),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    auto hr = createControls();
    if (FAILED(hr)) { DestroyWindow(window_); return hr; }
    hr = createBrowser();
    if (FAILED(hr)) { DestroyWindow(window_); return hr; }
    layout();
    hr = navigate(location.empty() ? (preferences_.useWindowsStartup ? windowsDefaultStartupLocation() : preferences_.startupLocation) : location);
    if (FAILED(hr)) DestroyWindow(window_);
    return hr;
}
HRESULT ExplorerApp::createBrowser() {
    auto hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser_));
    if (FAILED(hr)) return hr;
    ComPtr<IObjectWithSite> site;
    hr = browser_.As(&site);
    if (SUCCEEDED(hr)) hr = site->SetSite(static_cast<IServiceProvider*>(this));
    if (FAILED(hr)) { browser_.Reset(); return hr; }
    hr = browser_->Advise(static_cast<IExplorerBrowserEvents*>(this), &adviseCookie_);
    if (FAILED(hr)) { destroyBrowser(); return hr; }
    hr=browser_->SetOptions(static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_SHOWFRAMES | EBO_NOTRAVELLOG | EBO_NOBORDER |
        (headless_ || !saveLocalView_ ? EBO_NOPERSISTVIEWSTATE : 0)));
    if(FAILED(hr)){destroyBrowser();return hr;}
    hr=browser_->SetPropertyBag(L"WindowsExplorer.Native");
    if(FAILED(hr)){destroyBrowser();return hr;}
    FOLDERSETTINGS settings{static_cast<UINT>(FVM_AUTO), FWF_AUTOARRANGE};
    RECT rect{0, px(164), px(1100), px(700)};
    hr = browser_->Initialize(window_, &rect, &settings);
    browserInitialized_ = SUCCEEDED(hr);
    if (FAILED(hr)) { destroyBrowser(); return hr; }
    ComPtr<IFolderFilterSite> filterSite;
    hr = browser_.As(&filterSite);
    if (SUCCEEDED(hr)) hr = filterSite->SetFilter(static_cast<IFolderFilter*>(this));
    if (FAILED(hr)) { destroyBrowser(); return hr; }
    browser_->SetEmptyText(L"This folder is empty.");
    return S_OK;
}
void ExplorerApp::destroyBrowser() {
    cancelCommandStates();
    cancelFrequentPlaces();
    if(breadcrumbTask_) {breadcrumbTask_->cancel();breadcrumbTask_.reset();}
    if(closing_) {
        shutdownStatus_=drainStaWorkers(5000);
        if(FAILED(shutdownStatus_))return;
        namespaceActions_.reset(true);backgroundActions_.reset(true);archiveActions_.reset(true);
    }
    extractDestinations_.reset();
    newItemTypes_.reset();ribbonCommandChildren_.clear();ribbonCommandPaths_.clear();cancelFrequentPlaces();
    if(window_)KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();
    if (breadcrumbDrop_) { breadcrumbDrop_->revokeWindow(); breadcrumbDrop_.Reset(); }
    if(view_) {ComPtr<IObjectWithSite> site;if(SUCCEEDED(view_.As(&site)))site->SetSite(nullptr);}
    folderView_.Reset(); view_.Reset();
    if (browser_) {
        ComPtr<IFolderFilterSite> filterSite;
        if (SUCCEEDED(browser_.As(&filterSite))) filterSite->SetFilter(nullptr);
        if (adviseCookie_) { browser_->Unadvise(adviseCookie_); adviseCookie_ = 0; }
        ComPtr<IObjectWithSite> site;
        if (SUCCEEDED(browser_.As(&site))) site->SetSite(nullptr);
        if (browserInitialized_) { browser_->Destroy(); browserInitialized_ = false; }
        browser_.Reset();
    }
}
HRESULT ExplorerApp::recreateBrowser() {
    Pidl location(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
    destroyBrowser();
    const auto hr = createBrowser();
    layout();
    if (FAILED(hr)) return hr;
    return location ? browser_->BrowseToIDList(location.get(), SBSP_ABSOLUTE) :
        navigate(preferences_.useWindowsStartup ? windowsDefaultStartupLocation() : preferences_.startupLocation);
}
HRESULT ExplorerApp::navigate(const std::wstring& location, bool typedAddress) {
    if (!browser_) return E_UNEXPECTED;
    pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
    auto target = trim(expandEnvironment(location));
    if (target.size() > 1 && target.front() == L'"' && target.back() == L'"') target = target.substr(1, target.size() - 2);
    if (target.empty()) return E_INVALIDARG;
    // Relative filesystem paths resolve against the current folder, never the process working directory.
    if (target.find(L':') == std::wstring::npos && !target.starts_with(L"\\\\") && !currentLocation_.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(currentFolder(folder))) {
            auto path = itemName(folder.Get(), SIGDN_FILESYSPATH);
            if (!path.empty()) target = (std::filesystem::path(path) / target).lexically_normal().wstring();
        }
    }
    PIDLIST_ABSOLUTE raw = nullptr;
    const auto hr = SHParseDisplayName(target.c_str(), nullptr, &raw, SFGAO_FOLDER, nullptr);
    Pidl pidl(raw);
    if (FAILED(hr)) {
        if(!typedAddress)return hr;
        ComPtr<IShellItem> current;currentFolder(current);
        return launchTypedAddress(window_,location,headless_,{},physicalDirectory_?itemName(current.Get(),SIGDN_FILESYSPATH):std::wstring{});
    }
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromIDList(pidl.get(), IID_PPV_ARGS(&item)))) {
        SFGAOF attributes = 0;
        const auto attributeRead=item->GetAttributes(SFGAO_FOLDER, &attributes);
        if(FAILED(attributeRead))return attributeRead;
        if (!(attributes & SFGAO_FOLDER)) return headless_ ? (typedAddress?E_ACCESSDENIED:HRESULT_FROM_WIN32(ERROR_DIRECTORY)) : shellExecute(window_, target.c_str());
    }
    pendingHistory_ = -1;
    if(typedAddress&&typedAddressHistoryAllowed_) {
        pendingTypedAddress_=location;
        pendingTypedAddressTarget_.reset(ILCloneFull(pidl.get()));
        if(!pendingTypedAddressTarget_) {pendingTypedAddress_.clear();return E_OUTOFMEMORY;}
    }
    // An explicit address/navigation request also invalidates pending edit
    // work when the native browser reuses the current folder without events.
    cancelLiveSearch();
    const auto browseResult=browser_->BrowseToIDList(pidl.get(), SBSP_ABSOLUTE);
    if(FAILED(browseResult)){pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();}
    else if(!navigating_&&currentPidl_&&ILIsEqual(currentPidl_.get(),pidl.get()))rememberAddressNavigation(currentPidl_.get());
    return browseResult;
}
HRESULT ExplorerApp::OnNavigationPending(PCIDLIST_ABSOLUTE pidl) {
    if(closing_)return E_ABORT;
    LiveSearchDispatchScope navigationCallback(*this);
    const bool expectedLive=pendingLiveSearchTarget_&&ILIsEqual(pendingLiveSearchTarget_.get(),pidl);
    const bool expectedDirect=pendingDirectSearchTarget_&&ILIsEqual(pendingDirectSearchTarget_.get(),pidl);
    if(!expectedLive&&!expectedDirect)cancelLiveSearch();
    // Preserve the actual search view before the browser replaces it. A
    // provider that cannot expose its layout still retains the last snapshot.
    if(searchActive_&&folderView_)captureActiveSearchPresentation();
    if(pendingTypedAddressTarget_&&!ILIsEqual(pendingTypedAddressTarget_.get(),pidl)) {
        pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
    }
    pendingPidl_.reset(ILCloneFull(pidl));
    if (selectionDestination_ && !ILIsEqual(selectionDestination_.get(), pidl)) {
        selectionDestination_.reset(); selectionChild_.reset(); selectionTarget_.Reset();
    }
    if (pendingHistory_ >= 0 && !ILIsEqual(history_[pendingHistory_].get(), pidl)) pendingHistory_ = -1;
    const auto search = std::find_if(searchLocations_.begin(), searchLocations_.end(),
                                    [&](const SearchLocation& item) { return ILIsEqual(item.location.get(), pidl); });
    pendingSearchActive_ = search != searchLocations_.end();
    pendingSearchBackground_ = pendingSearchActive_ || isExternalSearch(pidl);
    cancelCommandStates();extractDestinations_.reset();KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();
    if (breadcrumbTask_) { breadcrumbTask_->cancel(); breadcrumbTask_.reset(); KillTimer(window_, 2); }
    navigating_ = true; navigationStarted_ = GetTickCount64();
    setStatus(L"Loading…"); return S_OK;
}
HRESULT ExplorerApp::OnViewCreated(IShellView* view) {
    if(closing_)return E_ABORT;
    LiveSearchDispatchScope navigationCallback(*this);
    view_ = view;
    folderView_.Reset();
    if (view) view->QueryInterface(IID_PPV_ARGS(&folderView_));
    PostMessageW(window_, DeferredView, 0, 0);
    return S_OK;
}
HRESULT ExplorerApp::OnNavigationComplete(PCIDLIST_ABSOLUTE pidl) {
    if(closing_)return S_OK;
    LiveSearchDispatchScope navigationCallback(*this);
    const bool liveNavigation=pendingLiveSearch_&&pendingLiveSearchTarget_&&ILIsEqual(pendingLiveSearchTarget_.get(),pidl);
    const bool liveQuery=liveNavigation&&pendingLiveSearch_->kind==LiveSearchKind::Query;
    const bool directNavigation=pendingDirectSearchTarget_&&ILIsEqual(pendingDirectSearchTarget_.get(),pidl);
    const auto directRevision=pendingDirectSearchRevision_;
    navigating_ = false;
    selectionStateDirty_ = namespaceDirty_ = true;
    lastNavigationMs_ = GetTickCount64() - navigationStarted_;
    ++navigationCount_;
    const auto search = std::find_if(searchLocations_.rbegin(), searchLocations_.rend(),
                                    [&](const SearchLocation& item) { return ILIsEqual(item.location.get(), pidl); });
    searchActive_ = search != searchLocations_.rend();
    searchBackground_ = searchActive_ || isExternalSearch(pidl);
    searchPresentation_.reset();
    searchPresentationPending_=false;
    searchPresentationStatus_=S_OK;
    std::wstring completedExplicitQuery;
    if (searchActive_) {
        searchScope_.reset(ILCloneFull(search->scope.get()));
        searchScopes_=search->scopes;
        searchScopeRules_=search->scopeRules;
        activeQuery_ = search->query;
        searchRecursive_ = search->recursive;
        searchBase_ = search->base;
        searchFilters_ = search->filters;
        searchPresentation_=search->presentation;
        if(search->rememberOnComplete&&!search->remembered) {
            search->remembered=true;completedExplicitQuery=search->query;
        }
    } else if (searchBackground_) {
        searchScopes_.Reset();
        searchScopeRules_.clear();
        const auto path = pidlName(pidl, SIGDN_FILESYSPATH);
        SavedSearchMetadata metadata;
        if (!path.empty() && SUCCEEDED(readSavedSearch(std::filesystem::path(path), &metadata))) {
            PIDLIST_ABSOLUTE rawScope = nullptr;
            const auto scopeResult = SHGetIDListFromObject(metadata.scope.Get(), &rawScope);
            Pidl scope(rawScope);
            if (SUCCEEDED(scopeResult) && scope) {
                searchScope_ = std::move(scope);
                searchScopes_ = std::move(metadata.scopes);
                searchScopeRules_=std::move(metadata.scopeRules);
                activeQuery_ = std::move(metadata.query);
                searchBase_ = activeQuery_;
                searchFilters_ = {};
                searchRecursive_ = metadata.recursive;
                searchActive_ = true;
                searchPresentation_=std::move(metadata.presentation);
                if(!headless_) {
                    SearchViewPresentation actual=searchPresentation_.value_or(SearchViewPresentation{});
                    const auto presentationRead=loadSearchPresentationCompanion(std::filesystem::path(path),
                        searchPresentationDirectory(),&actual);
                    if(presentationRead==S_OK)searchPresentation_=std::move(actual);
                    else if(FAILED(presentationRead))searchPresentationStatus_=presentationRead;
                }
            }
        }
    } else {searchScopes_.Reset();searchScopeRules_.clear();}
    if(searchActive_) {
        const auto remembered=std::find_if(searchPresentationLocations_.rbegin(),searchPresentationLocations_.rend(),
            [&](const SearchPresentationLocation& entry){return ILIsEqual(entry.location.get(),pidl);});
        if(remembered!=searchPresentationLocations_.rend())searchPresentation_=remembered->presentation;
    }
    searchPresentationPending_=searchPresentation_.has_value()||FAILED(searchPresentationStatus_);
    pendingSearchActive_ = searchActive_;
    pendingSearchBackground_ = searchBackground_;
    library_ = ShellLibrary{};
    if (!searchBackground_ && hasNativeItemType(pidl, L".library-ms")) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item)))) {
            // Request the real capability without committing any changes.
            // Headless activation/Commit guards remain enforced separately.
            if (FAILED(ShellLibrary::load(item.Get(), true, library_)))
                ShellLibrary::load(item.Get(), false, library_);
        }
    }
    currentPidl_.reset(ILCloneFull(pidl));
    pendingPidl_.reset();
    filesystemFolder_ = physicalDirectory_ = false;
    ComPtr<IShellItem> currentItem;
    if (SUCCEEDED(currentFolder(currentItem))) {
        SFGAOF attributes = 0;
        filesystemFolder_ = SUCCEEDED(currentItem->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) &&
            (attributes & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) == (SFGAO_FILESYSTEM | SFGAO_FOLDER);
        physicalDirectory_ = filesystemFolder_ && isPhysicalDirectory(currentItem.Get());
    }
    if (selectionDestination_ && ILIsEqual(selectionDestination_.get(), pidl)) {
        selectionDeadline_ = GetTickCount64() + 5000;
        selectionRetryAt_ = 0;
    }
    currentLocation_ = pidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING);
    currentName_ = pidlName(pidl, SIGDN_NORMALDISPLAY);
    quickAccessLocation_=librariesRoot_=false;
    if(currentItem) {
        ComPtr<IShellItem> home,libraries;int comparison=1;
        if(SUCCEEDED(SHCreateItemFromParsingName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}",nullptr,IID_PPV_ARGS(&home)))&&
           SUCCEEDED(currentItem->Compare(home.Get(),SICHINT_CANONICAL,&comparison)))quickAccessLocation_=comparison==0;
        comparison=1;
        if(SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Libraries,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&libraries)))&&
           SUCCEEDED(currentItem->Compare(libraries.Get(),SICHINT_CANONICAL,&comparison)))librariesRoot_=comparison==0;
    }
    if(liveQuery&&liveSearchHistoryIndex_>=0&&liveSearchHistoryIndex_<static_cast<int>(history_.size())) {
        history_.resize(static_cast<size_t>(liveSearchHistoryIndex_+1));
        history_[liveSearchHistoryIndex_].reset(ILCloneFull(pidl));
        historyIndex_=liveSearchHistoryIndex_;pendingHistory_=-1;
    }
    else if (pendingHistory_ >= 0 && ILIsEqual(history_[pendingHistory_].get(), pidl)) { historyIndex_ = pendingHistory_; pendingHistory_ = -1; }
    else if (historyIndex_ < 0 || !ILIsEqual(history_[historyIndex_].get(), pidl)) {
        pendingHistory_ = -1;
        history_.resize(static_cast<size_t>(historyIndex_ + 1));
        history_.emplace_back(ILCloneFull(pidl));
        historyIndex_ = static_cast<int>(history_.size()) - 1;
        if (history_.size() > 100) { history_.erase(history_.begin()); --historyIndex_; }
    }
    if(liveQuery)liveSearchHistoryIndex_=historyIndex_;
    updateFrameTitle();
    SetWindowTextW(address_, currentLocation_.c_str());
    SendMessageW(search_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>((L"Search " + currentName_).c_str()));
    if(liveNavigation)completeLiveSearchNavigation(pidl,S_OK);
    else if(directNavigation) {
        // The factory may pump a real edit after the chosen command has
        // started. Its completion must leave that newer literal and request.
        if(searchInteractionRevision_==directRevision)setSearchText(searchActive_?activeQuery_:L"");
        else if(pendingDirectSearchTarget_&&pendingDirectSearchRevision_==directRevision&&
                ILIsEqual(pendingDirectSearchTarget_.get(),pidl))pendingDirectSearchTarget_.reset();
    } else setSearchText(searchActive_?activeQuery_:L"");
    updateBreadcrumbs();
    updateContextTabs();
    if(!completedExplicitQuery.empty())rememberQuery(completedExplicitQuery);
    rememberAddressNavigation(pidl);
    PostMessageW(window_, DeferredView, 0, 0);
    scheduleDeferredUpdate();
    return S_OK;
}
HRESULT ExplorerApp::OnNavigationFailed(PCIDLIST_ABSOLUTE pidl) {
    if(closing_)return S_OK;
    LiveSearchDispatchScope navigationCallback(*this);
    pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
    pendingPidl_.reset();
    navigating_ = false; pendingHistory_ = -1;
    completeLiveSearchNavigation(pidl,E_FAIL);
    if(pendingDirectSearchTarget_&&ILIsEqual(pendingDirectSearchTarget_.get(),pidl))pendingDirectSearchTarget_.reset();
    selectionStateDirty_=namespaceDirty_=true;scheduleDeferredUpdate();
    selectionDestination_.reset(); selectionChild_.reset(); selectionTarget_.Reset();
    pendingSearchActive_ = searchActive_;
    pendingSearchBackground_ = searchBackground_;
    lastError_ = L"This location could not be opened. Check its availability and your permissions.";
    setStatus(lastError_);
    return S_OK;
}
HRESULT ExplorerApp::OnStateChange(IShellView*, ULONG change) {
    // Ribbon popup focus does not change the original view selection. Retain
    // its native command snapshot until selection, rename or state changes.
    if(change==CDBOSC_SETFOCUS||change==CDBOSC_KILLFOCUS)return S_OK;
    selectionStateDirty_ = namespaceDirty_ = true;
    scheduleDeferredUpdate(); return S_OK;
}
void ExplorerApp::scheduleDeferredUpdate() {
    if(commandRefreshActive_) {commandRefreshPending_=true;return;}
    if(deferredUpdateQueued_||closing_||!window_)return;
    deferredUpdateQueued_=PostMessageW(window_,DeferredUpdate,0,0)!=FALSE;
}
void ExplorerApp::deferCommandRefresh() {
    commandRefreshPending_=true;
    if(headless_)++deferredCommandUpdates_;
}
void ExplorerApp::finishCommandRefresh() noexcept {
    commandRefreshActive_=false;
    const bool cancelled=commandStatesCancelPending_;
    commandStatesCancelPending_=false;
    if(cancelled) {
        try {cancelCommandStatesImpl();}
        catch(...) {namespaceDirty_=true;}
    }
    const bool again=commandRefreshPending_||namespaceDirty_||selectionStateDirty_;
    commandRefreshPending_=false;
    const bool items=commandItemsRefreshPending_;
    commandItemsRefreshPending_=false;
    if(items&&!closing_) {
        try {ribbon_.invalidateItems();}
        catch(...) {namespaceDirty_=true;}
    }
    if(again&&!closing_&&!navigating_)scheduleDeferredUpdate();
}
double ExplorerApp::commandTimingNow() {
    if(!headless_)return 0;
    static const double frequency=[] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value)&&value.QuadPart>0?static_cast<double>(value.QuadPart):0;
    }();
    LARGE_INTEGER value{};
    if(!frequency||!QueryPerformanceCounter(&value)){commandTimings_.status=E_FAIL;return 0;}
    if(commandTimings_.status==E_PENDING)commandTimings_.status=S_OK;
    return static_cast<double>(value.QuadPart)*1000.0/frequency;
}

HRESULT ExplorerApp::createControls() {
    auto control = [&](const wchar_t* type, const wchar_t* label, DWORD style, UINT id) {
        auto hwnd = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                   window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
        if (hwnd) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return hwnd;
    };
    nav_ = control(TOOLBARCLASSNAMEW, L"Navigation", TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 901);
    SendMessageW(nav_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    auto navButton = [&](UINT id, const wchar_t* label) {
        TBBUTTON button{}; button.iBitmap = I_IMAGENONE; button.idCommand = id;
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_BUTTON;
        button.iString = reinterpret_cast<INT_PTR>(label);
        SendMessageW(nav_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button));
    };
    navButton(Back, L"Back"); navButton(Forward, L"Forward"); navButton(HistoryMenu, L"Recent locations"); navButton(Up, L"Up");
    breadcrumbs_ = control(TOOLBARCLASSNAMEW, L"Location breadcrumbs", TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS |
        CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 902);
    SendMessageW(breadcrumbs_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(breadcrumbs_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_MIXEDBUTTONS);
    address_ = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, Address);
    ShowWindow(address_, SW_HIDE);
    SetWindowSubclass(address_, editProc, Address, reinterpret_cast<DWORD_PTR>(this));
    SHAutoComplete(address_, SHACF_FILESYSTEM | SHACF_URLALL);
    search_ = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, Search);
    SetWindowSubclass(search_, editProc, Search, reinterpret_cast<DWORD_PTR>(this));
    if(!headless_ && searchSuggestionsAllowed_) {
        searchSuggestions_.Attach(new SearchSuggestionList());
        searchSuggestions_->replace(recentSearches_);
        attachSearchSuggestions(search_,searchSuggestions_.Get(),false,&searchAutocomplete_);
    }
    status_ = control(STATUSCLASSNAMEW, L"Ready", SBARS_SIZEGRIP, 903);
    ShowWindow(status_, SW_HIDE);
    statusView_ = control(TOOLBARCLASSNAMEW, L"View shortcuts", TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
        CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 904);
    SendMessageW(statusView_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    for (const auto command : {ViewFirst + static_cast<UINT>(ViewMode::Details), ViewFirst + static_cast<UINT>(ViewMode::LargeIcons)}) {
        TBBUTTON button{}; button.iBitmap = I_IMAGENONE; button.idCommand = command;
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_CHECKGROUP;
        button.iString = reinterpret_cast<INT_PTR>(command == ViewFirst + 5 ? L"Details" : L"Large icons");
        SendMessageW(statusView_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button));
    }
    ShowWindow(statusView_, SW_HIDE);
    addressActions_ = control(TOOLBARCLASSNAMEW, L"Address actions", TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
        CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 905);
    SendMessageW(addressActions_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    TBBUTTON refresh{}; refresh.iBitmap=I_IMAGENONE; refresh.idCommand=Refresh;
    refresh.fsState=TBSTATE_ENABLED; refresh.fsStyle=BTNS_BUTTON; refresh.iString=reinterpret_cast<INT_PTR>(L"Refresh");
    SendMessageW(addressActions_,TB_ADDBUTTONS,1,reinterpret_cast<LPARAM>(&refresh));
    if (!nav_ || !breadcrumbs_ || !address_ || !search_ || !status_ || !statusView_ || !addressActions_) return HRESULT_FROM_WIN32(GetLastError());
    ComPtr<IAccPropServices> accessible;
    if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&accessible)))) {
        accessible->SetHwndPropStr(address_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, L"Address");
        accessible->SetHwndPropStr(search_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, L"Search");
    }
    applyChrome(nav_, breadcrumbs_, address_, search_, statusView_, addressActions_);
    RibbonCallbacks callbacks;
    callbacks.execute = [this](UINT command) { auto hr = executeRibbon(command); showError(hr, L"Command"); return hr; };
    callbacks.query = [this](UINT command) { return ribbonState(command); };
    callbacks.items = [this](UINT command) { return ribbonItems(command); };
    callbacks.executeItem = [this](UINT command, UINT item) { auto hr = executeRibbonItem(command, item); showError(hr, L"Command"); return hr; };
    callbacks.pinItem=[this](UINT item,bool pinned){const auto hr=pinFrequentPlace(item,pinned);showError(hr,L"Pin frequent place");return hr;};
    callbacks.heightChanged = [this](UINT) { layout(); };
    auto hr = ribbon_.initialize(window_, instance_, std::move(callbacks),requestedRibbonLayout_);
    if (FAILED(hr)) return hr;
    applyRibbonTheme(ribbon_.framework());
    applyWindowTheme(window_);
    bool nativeSettingsLoaded = false;
    if (!headless_) {
        const auto settings = preferencesPath();
        if (!settings.empty()) {
            const auto nativeSettings = settings.parent_path() / L"ribbon.bin";
            std::error_code error;
            if (std::filesystem::exists(nativeSettings, error)) nativeSettingsLoaded = SUCCEEDED(ribbon_.loadSettings(nativeSettings));
            else rebuildQuickAccess();
        }
    }
    if (!nativeSettingsLoaded) ribbon_.setMinimized(preferences_.ribbonCollapsed);
    else ribbon_.minimized(preferences_.ribbonCollapsed);
    ribbonCollapse_=control(L"BUTTON",L"Minimize the Ribbon",BS_OWNERDRAW|BS_FLAT|WS_TABSTOP,Collapse);
    if(!ribbonCollapse_)return HRESULT_FROM_WIN32(GetLastError());
    const auto collapseTheme=applyRibbonCollapseButton(ribbonCollapse_);
    if(FAILED(collapseTheme))return collapseTheme;
    ribbonCollapseTooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance_,nullptr);
    if(ribbonCollapseTooltip_) {
        ribbonCollapseTip_=L"Minimize the Ribbon (Ctrl+F1)";
        TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=window_;
        tool.uId=reinterpret_cast<UINT_PTR>(ribbonCollapse_);tool.lpszText=ribbonCollapseTip_.data();
        SendMessageW(ribbonCollapseTooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
    }
    updateRibbonCollapseButton();
    AddClipboardFormatListener(window_);
    refreshFrequentPlaces();
    SetTimer(window_, 1, 700, nullptr);
    return S_OK;
}

void ExplorerApp::rebuildRibbon() {
    if (!ribbon_.valid()) return;
    ribbon_.setMinimized(preferences_.ribbonCollapsed);
    ribbon_.invalidateState();
    ribbon_.flush();
    layout();
}
void ExplorerApp::rebuildQuickAccess() {
    if (!ribbon_.valid()) return;
    std::vector<UINT> commands(quickAccessModel_.commands().begin(), quickAccessModel_.commands().end());
    ribbon_.setQuickAccessCommands(commands);
    ribbon_.setQuickAccessBelow(quickAccessModel_.belowRibbon());
    ribbon_.invalidateState();
    ribbon_.flush();
    layout();
}
void ExplorerApp::layout() {
    if(closing_)return;
    if (!window_ || !nav_) return;
    RECT client{}; GetClientRect(window_, &client);
    const int width = client.right, height = client.bottom;
    const int y = static_cast<int>(ribbon_.height());
    updateRibbonCollapseButton();
    const int searchWidth = std::clamp(px(preferences_.searchWidth),px(90),std::max(px(90),width-px(220)));
    const int navWidth = px(106);
    MoveWindow(nav_, px(3), y + px(5), navWidth - px(3), px(30), TRUE);
    const int addressWidth = std::max(px(90), width - navWidth - searchWidth - px(24));
    MoveWindow(address_, navWidth, y + px(5), addressWidth, px(30), TRUE);
    MoveWindow(breadcrumbs_, navWidth, y + px(5), std::max(px(60), addressWidth - px(25)), px(30), TRUE);
    MoveWindow(addressActions_, navWidth + addressWidth - px(24), y + px(5), px(24), px(30), TRUE);
    fitBreadcrumbs(std::max(px(60),addressWidth-px(25)));
    MoveWindow(search_, navWidth + addressWidth + px(12), y + px(5), searchWidth, px(30), TRUE);
    ShowWindow(status_,SW_HIDE); ShowWindow(statusView_,SW_HIDE);
    if (browser_) {
        RECT viewRect{0, y + px(41), width, std::max(y + px(42), height)};
        browser_->SetRect(nullptr, viewRect);
    }
}
void ExplorerApp::updateNamespace() {
    if(commandRefreshActive_) {deferCommandRefresh();return;}
    CommandRefreshScope scope(*this);
    updateNamespaceImpl();
}
void ExplorerApp::updateNamespaceImpl() {
    if(closing_)return;
    if (!namespaceDirty_ || !currentPidl_ || navigating_) return;
    // Consume the snapshot request before entering COM. Notifications received
    // while a provider pumps messages request a subsequent owner iteration.
    namespaceDirty_=false;
    const auto preparationStarted=commandTimingNow();
    ++namespaceGeneration_;
    cancelCommandStatesImpl();
    extractDestinations_.reset();
    newItemTypes_.reset();
    ribbonCommandChildren_.clear();
    ribbonCommandPaths_.clear();
    NamespaceTarget target;
    currentFolder(target.folder);
    namespaceNetwork_=false;
    nativeLibraryFactoryReady_=false;
    if(target.folder) {
        ComPtr<IShellItem> network;int comparison=1;
        if(SUCCEEDED(SHGetKnownFolderItem(FOLDERID_NetworkFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&network)))&&
           SUCCEEDED(target.folder->Compare(network.Get(),SICHINT_CANONICAL,&comparison)))namespaceNetwork_=comparison==0;
    }
    selection(target.selection);
    selectionKinds_={};
    const auto kindsStarted=commandTimingNow();
    if(target.selection)namespaceSelectionKinds(target.selection.Get(),&selectionKinds_);
    const auto kindsCompleted=commandTimingNow();
    if(headless_)commandTimings_.selectionKindsMs+=kindsCompleted-kindsStarted;
    target.site = view_;
    target.completionMessage = NamespaceResult;
    const auto initialized = namespaceActions_.initialize(window_, target);
    auto background=target;background.selection.Reset();
    const auto backgroundInitialized=backgroundActions_.initialize(window_,background);
    archiveFolder_ = hasNativeItemType(currentPidl_.get(), L".zip");
    selectionArchive_ = false;
    if (target.selection && selectionCount_ == 1) {
        ComPtr<IShellItem> selected;
        if(SUCCEEDED(target.selection->GetItemAt(0,&selected)))selectionArchive_=itemHasFastType(selected.Get(),L".zip");
    }
    archiveTargetValid_ = false;
    archiveActions_.reset();
    if (archiveFolder_ && target.folder) {
        NamespaceTarget archiveTarget;
        archiveTarget.folder = target.folder;
        archiveTarget.site = target.site;
        archiveTarget.completionMessage = target.completionMessage;
        if (SUCCEEDED(SHCreateShellItemArrayFromShellItem(target.folder.Get(), IID_PPV_ARGS(&archiveTarget.selection))))
            archiveTargetValid_ = SUCCEEDED(archiveActions_.initialize(window_, archiveTarget));
    }
    std::map<UINT,AppCommandCapability> capabilities;
    const auto catalogStarted=commandTimingNow();
    if(headless_)commandTimings_.namespacePreparationMs+=(kindsStarted-preparationStarted)+(catalogStarted-kindsCompleted);
    if (SUCCEEDED(initialized)||SUCCEEDED(backgroundInitialized)) {
        const auto context = commandContext();
        for (const auto& binding : appCommandCatalog()) {
            AppCommandCapability capability;
            const bool useBackground=binding.scope==NamespaceMenuScope::Background;
            if(useBackground?FAILED(backgroundInitialized):FAILED(initialized))continue;
            auto& actions = binding.command == Extract && archiveTargetValid_ ? archiveActions_ : useBackground?backgroundActions_:namespaceActions_;
            if (SUCCEEDED(queryAppCommand(actions, binding.command, context, &capability)))
                capabilities.emplace(binding.command, std::move(capability));
            if(headless_&&headlessCommandReentryProbe_) {
                auto probe=std::move(headlessCommandReentryProbe_);
                headlessCommandReentryProbe_={};probe();
            }
        }
        if(librariesRoot_&&SUCCEEDED(backgroundInitialized)&&
           SUCCEEDED(backgroundActions_.queryCommandChildren(L"Windows.newitem",&newItemTypes_,NamespaceMenuScope::Background))&&
           newItemTypes_&&newItemTypes_->entries().size()==1) {
            const auto& child=newItemTypes_->entries().front();
            nativeLibraryFactoryReady_=SUCCEEDED(child.stateStatus)&&!(child.state&(ECS_DISABLED|ECS_HIDDEN))&&
                !(child.flags&(ECF_HASSUBCOMMANDS|ECF_ISSEPARATOR))&&child.children.empty();
        }
        NamespaceCommandMetadata open;
        if (SUCCEEDED(namespaceCommandMetadata(L"Windows.open", &open, target.selection.Get(), target.site.Get()))) {
            ribbon_.setCommandImageSpec(Open, open.icon);
            ribbon_.setCommandImageSpec(RibbonOpenMenu, open.icon);
        }
    }
    commandCapabilities_=std::move(capabilities);
    const auto enabled = [this](UINT id) {
        const auto found = commandCapabilities_.find(id);
        return found != commandCapabilities_.end() && found->second.enabled;
    };
    undoAvailable_ = enabled(Undo); redoAvailable_ = enabled(Redo);
    const auto catalogCompleted=commandTimingNow();
    if(headless_)commandTimings_.providerCatalogMs+=catalogCompleted-catalogStarted;
    ribbon_.invalidateItems();
    const auto invalidationCompleted=commandTimingNow();
    if(headless_)commandTimings_.ribbonInvalidationMs+=invalidationCompleted-catalogCompleted;
    if(!namespaceDirty_&&!selectionStateDirty_&&!navigating_)startPendingCommandStates();
    if(headless_)commandTimings_.stateTaskSchedulingMs+=commandTimingNow()-invalidationCompleted;
}
void ExplorerApp::updateContextTabs() {
    if(commandRefreshActive_) {deferCommandRefresh();return;}
    CommandRefreshScope scope(*this);
    updateContextTabsImpl();
}
void ExplorerApp::updateContextTabsImpl() {
    if (!ribbon_.valid()) return;
    updateNamespaceImpl();
    const auto contextStarted=commandTimingNow();
    contextPage_ = searchActive_ ? ContextPage::Search : library_.valid() ? ContextPage::Library : ContextPage::None;
    RibbonContext contexts = RibbonContext::None;
    if (searchBackground_) contexts = contexts | RibbonContext::Search;
    if (library_.valid()) contexts = contexts | RibbonContext::Library;
    const auto& facts = namespaceActions_.facts();
    ribbon_.setDriveType(facts.driveType);
    if (facts.images) contexts = contexts | RibbonContext::Picture;
    if (facts.driveRoot) contexts = contexts | RibbonContext::Drive;
    if (facts.recycleBin) contexts = contexts | RibbonContext::Recycle;
    if (facts.applications) contexts = contexts | RibbonContext::Application;
    if (facts.discImages) contexts = contexts | RibbonContext::DiscImage;
    if (selectionCount_ && (selectionAttributes_ & SFGAO_LINK)) contexts = contexts | RibbonContext::Shortcut;
    const bool compressed = archiveFolder_ || selectionArchive_;
    if (compressed) contexts = contexts | RibbonContext::Compressed;
    if (selectionKinds_.music) contexts = contexts | RibbonContext::Music;
    if (selectionKinds_.video) contexts = contexts | RibbonContext::Video;
    if (visualPage_ && *visualPage_ >= RibbonPictureTab && *visualPage_ <= RibbonDiscImageTab) {
        constexpr RibbonContext forced[]{RibbonContext::Picture,RibbonContext::Drive,RibbonContext::Compressed,
            RibbonContext::Search,RibbonContext::Library,RibbonContext::Recycle,RibbonContext::Application,
            RibbonContext::Music,RibbonContext::Video,RibbonContext::DiscImage};
        contexts = contexts | forced[*visualPage_ - RibbonPictureTab];
    }
    bool computer = commandContext().computer;
    bool network=namespaceNetwork_;
    if (visualPage_ && (*visualPage_ == RibbonHomeTab || *visualPage_ == RibbonShareTab)) computer = false;
    if (visualPage_ && *visualPage_ == RibbonComputerTab) computer = true;
    if(visualPage_&&*visualPage_==RibbonNetworkTab) {network=true;computer=false;}
    if(visualPage_&&(*visualPage_==RibbonHomeTab||*visualPage_==RibbonShareTab||*visualPage_==RibbonComputerTab))network=false;
    const auto directory=commandCapabilities_.find(RibbonSearchActiveDirectory);
    const bool activeDirectory=directory!=commandCapabilities_.end()&&directory->second.enabled;
    if(computer!=ribbonComputer_||network!=ribbonNetwork_||(network&&activeDirectory!=ribbonNetworkActiveDirectory_)) {
        const auto hr=network?ribbon_.setNetworkMode(true,activeDirectory):ribbon_.setComputerMode(computer);
        if(SUCCEEDED(hr)){ribbonComputer_=computer;ribbonNetwork_=network;ribbonNetworkActiveDirectory_=activeDirectory;}
    }
    if (contexts != ribbonContexts_) {
        ribbonContexts_ = contexts;
        ribbon_.setContexts(contexts, searchBackground_ || library_.valid() || facts.recycleBin);
    }
    ribbon_.invalidateState();
    if(headless_)commandTimings_.contextMs+=commandTimingNow()-contextStarted;
}

HRESULT ExplorerApp::cycleFocus(bool backwards) {
    HWND nativeView = nullptr;
    if (view_) view_->GetWindow(&nativeView);
    HWND tree = nullptr;
    struct TreeSearch { HWND host; HWND view; HWND tree = nullptr; } treeSearch{window_, nativeView};
    EnumChildWindows(window_, [](HWND child, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<TreeSearch*>(data);
        wchar_t name[64]{}; GetClassNameW(child, name, 64);
        if (_wcsicmp(name, WC_TREEVIEWW) != 0 || (search.view && IsChild(search.view, child))) return TRUE;
        bool namespaceTree = false;
        for (auto current = child; current && current != search.host; current = GetParent(current)) {
            if (!(GetWindowLongPtrW(current, GWL_STYLE) & WS_VISIBLE)) return TRUE;
            GetClassNameW(current, name, 64);
            if (_wcsicmp(name, L"NamespaceTreeControl") == 0) namespaceTree = true;
        }
        if (namespaceTree) { search.tree = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&treeSearch));
    tree = treeSearch.tree;
    auto usable = [](HWND hwnd) { return hwnd && IsWindow(hwnd) && IsWindowEnabled(hwnd); };
    const auto focus = GetFocus();
    auto within = [&](HWND hwnd) { return hwnd && (focus == hwnd || IsChild(hwnd, focus)); };
    std::optional<FocusRegion> current;
    if (within(address_) || within(breadcrumbs_)) current = FocusRegion::Address;
    else if (within(search_)) current = FocusRegion::Search;
    else if (within(nativeView)) current = FocusRegion::FolderView;
    else if (within(tree)) current = FocusRegion::Navigation;
    else if (focus && ribbon_.valid() && IsChild(window_, focus))
        current = FocusRegion::CommandBand;
    const FocusAvailability available{usable(address_), usable(search_), usable(nativeView), ribbon_.valid(),
                                      preferences_.navigationPane && usable(tree)};
    const auto next = cycleFocusRegion(current, backwards, available);
    if (!next) return S_FALSE;
    switch (*next) {
    case FocusRegion::Address: editAddress(); break;
    case FocusRegion::Search: SetFocus(search_); break;
    case FocusRegion::FolderView: return view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
    case FocusRegion::CommandBand: {
        auto bar = FindWindowExW(window_, nullptr, L"UIRibbonCommandBar", nullptr);
        if (bar) SetFocus(bar);
        break;
    }
    case FocusRegion::Navigation: SetFocus(tree); break;
    }
    return S_OK;
}

HRESULT ExplorerApp::toggleFullscreen() {
    if (!fullscreen_) {
        windowStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
        if (!GetWindowPlacement(window_, &windowPlacement_) || !GetWindowRect(window_, &windowRect_))
            return HRESULT_FROM_WIN32(GetLastError());
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
            return HRESULT_FROM_WIN32(GetLastError());
        SetWindowLongPtrW(window_, GWL_STYLE, windowStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE | WS_MINIMIZE));
        if (!SetWindowPos(window_, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
                          monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
            SetWindowLongPtrW(window_, GWL_STYLE, windowStyle_); return HRESULT_FROM_WIN32(GetLastError());
        }
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, windowStyle_);
        // SetWindowPlacement can show a previously hidden top-level window.
        // Headless tests restore only its geometry, without changing visibility.
        const BOOL restored = headless_
            ? SetWindowPos(window_, nullptr, windowRect_.left, windowRect_.top, windowRect_.right - windowRect_.left,
                           windowRect_.bottom - windowRect_.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)
            : SetWindowPlacement(window_, &windowPlacement_);
        if (!restored) return HRESULT_FROM_WIN32(GetLastError());
        fullscreen_ = false;
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    layout(); return S_OK;
}

HRESULT ExplorerApp::sizeColumns() {
    if (!folderView_) return E_UNEXPECTED;
    ComPtr<IColumnManager> columns;
    auto hr = folderView_.As(&columns);
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = columns->GetColumnCount(CM_ENUM_VISIBLE, &count);
    if (FAILED(hr)) return hr;
    if (count > 1000) return E_UNEXPECTED;
    std::vector<PROPERTYKEY> keys(count);
    hr = columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), count);
    if (FAILED(hr)) return hr;
    CM_COLUMNINFO info{sizeof(info)}; info.dwMask = CM_MASK_WIDTH;
    info.uWidth = static_cast<UINT>(CM_WIDTH_AUTOSIZE);
    for (const auto& key : keys) {
        hr = columns->SetColumnInfo(key, &info);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT ExplorerApp::toggleColumn(const PROPERTYKEY& key) {
    if (!folderView_) return E_UNEXPECTED;
    if (IsEqualPropertyKey(key, PKEY_ItemNameDisplay)) return E_ACCESSDENIED;
    ComPtr<IColumnManager> columns;
    auto hr = folderView_.As(&columns);
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = columns->GetColumnCount(CM_ENUM_VISIBLE, &count);
    if (FAILED(hr)) return hr;
    if (count > 1000) return E_UNEXPECTED;
    std::vector<PROPERTYKEY> keys(count);
    hr = columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), count);
    if (FAILED(hr)) return hr;
    const auto found = std::find_if(keys.begin(), keys.end(), [&](const PROPERTYKEY& value) { return IsEqualPropertyKey(key, value); });
    if (found == keys.end()) keys.push_back(key);
    else keys.erase(found);
    if (keys.empty()) return E_INVALIDARG;
    std::vector<CM_COLUMNINFO> info(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        info[i].cbSize = sizeof(CM_COLUMNINFO); info[i].dwMask = CM_MASK_WIDTH | CM_MASK_STATE;
        if (FAILED(columns->GetColumnInfo(keys[i], &info[i]))) return E_INVALIDARG;
    }
    hr = columns->SetColumns(keys.data(), static_cast<UINT>(keys.size()));
    for (size_t i = 0; SUCCEEDED(hr) && i < keys.size(); ++i) {
        info[i].dwState |= CM_STATE_VISIBLE;
        hr = columns->SetColumnInfo(keys[i], &info[i]);
    }
    return hr;
}

HRESULT ExplorerApp::captureActiveSearchPresentation() {
    if(!searchActive_||!folderView_)return E_UNEXPECTED;
    SearchViewPresentation actual;
    const auto hr=captureSearchViewPresentation(folderView_.Get(),&actual);
    if(FAILED(hr))return hr;
    searchPresentation_=actual;
    if(currentPidl_) {
        const auto location=std::find_if(searchLocations_.rbegin(),searchLocations_.rend(),
            [&](const SearchLocation& entry){return ILIsEqual(entry.location.get(),currentPidl_.get());});
        if(location!=searchLocations_.rend())location->presentation=actual;
        const auto remembered=std::find_if(searchPresentationLocations_.rbegin(),searchPresentationLocations_.rend(),
            [&](const SearchPresentationLocation& entry){return ILIsEqual(entry.location.get(),currentPidl_.get());});
        if(remembered!=searchPresentationLocations_.rend())remembered->presentation=std::move(actual);
        else {
            Pidl locationCopy(ILCloneFull(currentPidl_.get()));
            if(!locationCopy)return E_OUTOFMEMORY;
            searchPresentationLocations_.push_back({std::move(locationCopy),std::move(actual)});
            if(searchPresentationLocations_.size()>100)searchPresentationLocations_.erase(searchPresentationLocations_.begin());
        }
    }
    return S_OK;
}
HRESULT ExplorerApp::saveSearch() {
    if (headless_) return E_ACCESSDENIED;
    if (!searchActive_ || !searchScope_ || activeQuery_.empty()) return E_UNEXPECTED;
    if(navigating_)return HRESULT_FROM_WIN32(ERROR_BUSY);
    cancelLiveSearch();
    ComPtr<IShellItem> scope;
    auto hr = SHCreateItemFromIDList(searchScope_.get(), IID_PPV_ARGS(&scope));
    if (FAILED(hr)) return hr;
    ComPtr<IShellItemArray> scopes=searchScopes_;
    if(!scopes) {hr=SHCreateShellItemArrayFromShellItem(scope.Get(),IID_PPV_ARGS(&scopes));if(FAILED(hr))return hr;}
    hr=captureActiveSearchPresentation();
    if(FAILED(hr))return hr;
    const auto actual=*searchPresentation_;
    SearchViewPresentation native;
    hr=nativeSearchViewPresentation(actual,&native);
    if(FAILED(hr))return hr;
    ComPtr<IFileSaveDialog> dialog;
    hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return hr;
    const COMDLG_FILTERSPEC filter{L"Saved searches", L"*.search-ms"};
    FILEOPENDIALOGOPTIONS options{};
    hr=dialog->GetOptions(&options);
    if(SUCCEEDED(hr))hr=dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOREADONLYRETURN|FOS_OVERWRITEPROMPT);
    if(SUCCEEDED(hr))hr=dialog->SetFileTypes(1,&filter);
    if(SUCCEEDED(hr))hr=dialog->SetDefaultExtension(L"search-ms");
    if(SUCCEEDED(hr))hr=dialog->SetTitle(L"Save search");
    if(FAILED(hr))return hr;
    std::wstring name = activeQuery_.substr(0, 60);
    for (auto& ch : name) if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    name = trim(name);
    while (!name.empty() && name.back() == L'.') name.pop_back();
    if (!validLeafName(name + L".search-ms")) name = L"Search results";
    hr=dialog->SetFileName((name + L".search-ms").c_str());
    if(FAILED(hr))return hr;
    ComPtr<IShellItem> savedSearches;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_SavedSearches, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&savedSearches))))
        dialog->SetDefaultFolder(savedSearches.Get());
    hr = dialog->Show(window_);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> output;
    hr = dialog->GetResult(&output);
    if (FAILED(hr)) return hr;
    const auto path = itemName(output.Get(), SIGDN_FILESYSPATH);
    if(path.empty())return E_INVALIDARG;
    hr=searchScopeRules_.empty()?explorer::saveSearchForScopes(activeQuery_,scopes.Get(),searchRecursive_,std::filesystem::path(path),SearchSaveMode::UserConfirmed,&native):
        explorer::saveSearchForScopeRules(activeQuery_,searchScopeRules_,std::filesystem::path(path),SearchSaveMode::UserConfirmed,&native);
    if(FAILED(hr))return hr;
    // The query is already published. Report a companion failure separately
    // so the user knows the saved search remains usable in Windows Explorer.
    hr=saveSearchPresentationCompanion(std::filesystem::path(path),searchPresentationDirectory(),actual);
    if(FAILED(hr)) {
        showError(hr,L"Search saved, but its view could not be saved");
        return S_OK;
    }
    return S_OK;
}

HRESULT ExplorerApp::openFileLocation() {
    if (!searchBackground_ || !browser_) return E_UNEXPECTED;
    ComPtr<IShellItemArray> items;
    auto hr = selection(items);
    if (FAILED(hr)) return hr;
    if (!items) return E_INVALIDARG;
    DWORD count = 0;
    hr = items->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (count != 1) return E_INVALIDARG;
    ComPtr<IShellItem> item, parent;
    hr = items->GetItemAt(0, &item);
    if (FAILED(hr)) return hr;
    // A result selected in a native search view can have the search folder as
    // its Shell parent. Resolve its filesystem parsing name first, so this
    // command opens the containing directory instead of the result container.
    const auto path = itemName(item.Get(), SIGDN_FILESYSPATH);
    if (path.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    ComPtr<IShellItem> filesystemItem;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&filesystemItem));
    if (FAILED(hr)) return hr;
    item = filesystemItem;
    if (SUCCEEDED(hr)) hr = item->GetParent(&parent);
    if (FAILED(hr)) return hr;
    PIDLIST_ABSOLUTE raw = nullptr;
    hr = SHGetIDListFromObject(item.Get(), &raw);
    Pidl absolute(raw);
    if (FAILED(hr)) return hr;
    Pidl child(ILClone(ILFindLastID(absolute.get())));
    raw = nullptr;
    hr = SHGetIDListFromObject(parent.Get(), &raw);
    Pidl destination(raw);
    if (FAILED(hr)) return hr;
    if (!child || ILIsEmpty(child.get()) || !destination) return E_UNEXPECTED;
    // Keep the child PIDL tied to its actual parent; never pass a result
    // container's relative child identifier into a filesystem view.
    Pidl identifierParent(ILCloneFull(absolute.get()));
    if (!identifierParent || !ILRemoveLastID(identifierParent.get()) ||
        !ILIsEqual(identifierParent.get(), destination.get())) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    selectionDestination_ = std::move(destination);
    selectionChild_ = std::move(child);
    selectionTarget_ = item;
    selectionDeadline_ = selectionRetryAt_ = 0;
    pendingHistory_ = -1;
    hr = browser_->BrowseToObject(parent.Get(), SBSP_ABSOLUTE);
    if (FAILED(hr)) { selectionDestination_.reset(); selectionChild_.reset(); selectionTarget_.Reset(); }
    return hr;
}

HRESULT ExplorerApp::newItemMenu() {
    if (headless_) return E_ACCESSDENIED;
    if (navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder);
    if (FAILED(hr)) return hr;
    NativeContextMenu menu;
    hr = menu.createNewItems(window_, folder.Get(), view_.Get());
    if (FAILED(hr)) return hr;
    const HWND anchor = GetDlgItem(window_, NewItems);
    RECT rect{};
    if (anchor) GetWindowRect(anchor, &rect);
    else { POINT point{}; GetCursorPos(&point); rect.left = point.x; rect.bottom = point.y; }
    struct ActiveMenu {
        NativeContextMenu*& slot;
        NativeContextMenu* previous;
        ActiveMenu(NativeContextMenu*& target, NativeContextMenu* value) : slot(target), previous(target) { slot = value; }
        ~ActiveMenu() { slot = previous; }
    } active(activeContextMenu_, &menu);
    const POINT point{rect.left, rect.bottom};
    const auto selected = TrackPopupMenuEx(menu.popup(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        point.x, point.y, window_, nullptr);
    return selected ? menu.invoke(selected, point, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
        (GetKeyState(VK_SHIFT) & 0x8000) != 0) : S_FALSE;
}

HRESULT ExplorerApp::shareFiles() {
    if (headless_) return E_ACCESSDENIED;
    ComPtr<IShellItemArray> items;
    auto hr = selection(items);
    if (FAILED(hr)) return hr;
    SharePayload payload;
    hr = makeSharePayload(items.Get(), L"Share files", payload);
    if (FAILED(hr)) return hr;
    if (!nativeShare_.ready()) hr = nativeShare_.initialize(window_, ShareResult);
    return FAILED(hr) ? hr : nativeShare_.show(payload);
}

HRESULT ExplorerApp::newLibrary() {
    if (headless_) return E_ACCESSDENIED;
    updateNamespace();
    if(librariesRoot_) {
        CommandRefreshScope nativeCommand(*this);
        if(!nativeLibraryFactoryReady_||!newItemTypes_)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return newItemTypes_->invoke(0,false);
    }
    ComPtr<IFileSaveDialog> dialog;
    auto hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return hr;
    const COMDLG_FILTERSPEC filter{L"Windows libraries", L"*.library-ms"};
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN);
    dialog->SetFileTypes(1, &filter); dialog->SetDefaultExtension(L"library-ms");
    dialog->SetTitle(L"Create a new library"); dialog->SetFileName(L"New library.library-ms");
    ComPtr<IShellItem> defaultFolder;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Libraries, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&defaultFolder))))
        dialog->SetDefaultFolder(defaultFolder.Get());
    hr = dialog->Show(window_); if (FAILED(hr)) return hr;
    ComPtr<IShellItem> output;
    hr = dialog->GetResult(&output); if (FAILED(hr)) return hr;
    const std::filesystem::path path(itemName(output.Get(), SIGDN_FILESYSPATH));
    if (path.empty() || _wcsicmp(path.extension().c_str(), L".library-ms") != 0) return E_INVALIDARG;
    ShellLibrary library;
    hr = ShellLibrary::create(library); if (FAILED(hr)) return hr;
    hr = library.save(path.parent_path(), path.stem().wstring(), output);
    if (SUCCEEDED(hr)) {
        library = ShellLibrary{};
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, path.c_str(), nullptr);
        hr = browser_->BrowseToObject(output.Get(), SBSP_ABSOLUTE);
    }
    return hr;
}

HRESULT ExplorerApp::includeLibraryFolder() {
    if (headless_) return E_ACCESSDENIED;
    if (!library_.valid() || !library_.writable()) return E_ACCESSDENIED;
    Pidl target(ILCloneFull(currentPidl_.get()));
    if (!target) return E_OUTOFMEMORY;
    const auto generation = navigationCount_;
    ComPtr<IFileOpenDialog> dialog;
    auto hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return hr;
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Include a folder in this library");
    ComPtr<IShellItem> defaultFolder;
    if (SUCCEEDED(library_.defaultSaveFolder(defaultFolder))) dialog->SetDefaultFolder(defaultFolder.Get());
    hr = dialog->Show(window_); if (FAILED(hr)) return hr;
    if (navigating_ || generation != navigationCount_ || !currentPidl_ || !ILIsEqual(target.get(), currentPidl_.get()))
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    ComPtr<IShellItem> folder;
    hr = dialog->GetResult(&folder); if (FAILED(hr)) return hr;
    hr = library_.addFolder(std::filesystem::path(itemName(folder.Get(), SIGDN_FILESYSPATH)));
    return FAILED(hr) ? hr : commitLibrary();
}

HRESULT ExplorerApp::commitLibrary() {
    if (headless_) return E_ACCESSDENIED;
    const auto hr = library_.commit();
    if (SUCCEEDED(hr)) {
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_IDLIST, currentPidl_.get(), nullptr);
        if (view_) view_->Refresh();
        updateCommands();
    } else {
        // Discard uncommitted edits so a later action cannot accidentally save
        // an operation that was reported as failed.
        reloadLibrary(); updateContextTabs(); updateCommands();
    }
    return hr;
}

void ExplorerApp::reloadLibrary() {
    library_ = ShellLibrary{};
    ComPtr<IShellItem> item;
    if (!currentPidl_ || !hasNativeItemType(currentPidl_.get(), L".library-ms") ||
        FAILED(SHCreateItemFromIDList(currentPidl_.get(), IID_PPV_ARGS(&item)))) return;
    if (FAILED(ShellLibrary::load(item.Get(), !headless_, library_))) ShellLibrary::load(item.Get(), false, library_);
}

void ExplorerApp::applyPendingSelection() {
    if (!selectionDestination_ || !selectionChild_ || !selectionTarget_ || !selectionDeadline_ || navigating_ || !view_ ||
        !currentPidl_ || !ILIsEqual(selectionDestination_.get(), currentPidl_.get())) return;
    auto selectedExactly = [&] {
        ComPtr<IShellItemArray> items;
        ComPtr<IShellItem> item;
        DWORD count = 0; int comparison = 1;
        return SUCCEEDED(selection(items)) && items && SUCCEEDED(items->GetCount(&count)) && count == 1 &&
            SUCCEEDED(items->GetItemAt(0, &item)) && SUCCEEDED(item->Compare(selectionTarget_.Get(), SICHINT_CANONICAL, &comparison)) &&
            comparison == 0;
    };
    if (selectedExactly()) {
        selectionDestination_.reset(); selectionChild_.reset(); selectionTarget_.Reset(); return;
    }
    const auto now = GetTickCount64();
    if (now < selectionRetryAt_) return;
    selectionRetryAt_ = now + 100;
    auto hr = view_->SelectItem(selectionChild_.get(),
        SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED);
    const bool confirmed = selectedExactly();
    if (confirmed || now >= selectionDeadline_) {
        selectionDestination_.reset(); selectionChild_.reset(); selectionTarget_.Reset();
        if (!confirmed) showError(FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Select file in its location");
    }
}

void ExplorerApp::cancelLiveSearch() {
    if(window_)KillTimer(window_,6);
    ++searchInteractionRevision_;
    liveSearchPolicy_.cancel();pendingLiveSearch_.reset();pendingLiveSearchTarget_.reset();
    pendingDirectSearchTarget_.reset();
    liveSearchOrigin_.reset();liveSearchHistoryIndex_=-1;
}
void ExplorerApp::setSearchText(const std::wstring& text,bool synchronizePolicy) {
    if(synchronizePolicy) {
        cancelLiveSearch();
        liveSearchPolicy_.replaceText(text);
    }
    const bool previous=suppressSearchChanges_;suppressSearchChanges_=true;
    SetWindowTextW(search_,text.c_str());suppressSearchChanges_=previous;
}
void ExplorerApp::rememberQuery(const std::wstring& query) {
    if(!headless_&&!searchSuggestionsAllowed_)return;
    rememberSearch(recentSearches_,query);
    if(searchSuggestions_)searchSuggestions_->replace(recentSearches_);
    if(!headless_)saveSearchHistory(searchHistoryPath(),recentSearches_);
    ribbon_.invalidate(RecentSearches);
}
void ExplorerApp::scheduleLiveSearch() {
    if(!window_||closing_)return;
    const auto deadline=liveSearchPolicy_.deadline();
    if(!deadline) {KillTimer(window_,6);return;}
    const auto now=GetTickCount64();
    const auto delay=navigating_||commandRefreshActive_||liveSearchDispatchActive_?100ULL:
        std::clamp(*deadline>now?*deadline-now:10ULL,10ULL,250ULL);
    SetTimer(window_,6,static_cast<UINT>(delay),nullptr);
}
bool ExplorerApp::completeLiveSearchNavigation(PCIDLIST_ABSOLUTE target,HRESULT result) {
    if(!pendingLiveSearch_||!pendingLiveSearchTarget_||!target||
       !ILIsEqual(pendingLiveSearchTarget_.get(),target))return false;
    auto request=std::move(*pendingLiveSearch_);pendingLiveSearch_.reset();pendingLiveSearchTarget_.reset();
    const bool accepted=liveSearchPolicy_.finish(request,result);
    liveSearchStatus_=result;
    if(accepted&&SUCCEEDED(result)) {
        setSearchText(request.literal,false);
        if(request.kind==LiveSearchKind::Query&&request.explicitSubmit)rememberQuery(trim(request.literal));
        if(request.kind==LiveSearchKind::ReturnToOrigin) {liveSearchOrigin_.reset();liveSearchHistoryIndex_=-1;}
    }
    scheduleLiveSearch();return true;
}
HRESULT ExplorerApp::processLiveSearch() {
    if(closing_)return E_ABORT;
    if(navigating_||commandRefreshActive_||liveSearchDispatchActive_) {scheduleLiveSearch();return S_OK;}
    LiveSearchDispatchScope dispatch(*this);
    const auto request=liveSearchPolicy_.takeReady(GetTickCount64(),true);
    if(!request) {scheduleLiveSearch();return S_FALSE;}
    KillTimer(window_,6);
    if(request->kind==LiveSearchKind::ReturnToOrigin) {
        if(!liveSearchOrigin_&&searchActive_&&searchScope_)liveSearchOrigin_.reset(ILCloneFull(searchScope_.get()));
        if(!liveSearchOrigin_&&searchBackground_&&historyIndex_>0)
            liveSearchOrigin_.reset(ILCloneFull(history_[historyIndex_-1].get()));
        if(!liveSearchOrigin_) {
            liveSearchPolicy_.finish(*request,S_OK);setSearchText(L"",false);return S_OK;
        }
        pendingLiveSearch_=*request;pendingLiveSearchTarget_.reset(ILCloneFull(liveSearchOrigin_.get()));
        if(!pendingLiveSearchTarget_) {liveSearchPolicy_.finish(*request,E_OUTOFMEMORY);pendingLiveSearch_.reset();return E_OUTOFMEMORY;}
        pendingHistory_=-1;
        for(size_t index=0;index<history_.size();++index)
            if(ILIsEqual(history_[index].get(),liveSearchOrigin_.get()))pendingHistory_=static_cast<int>(index);
        liveSearchStatus_=browser_?browser_->BrowseToIDList(liveSearchOrigin_.get(),SBSP_ABSOLUTE):E_UNEXPECTED;
    } else {
        if(!currentPidl_) {liveSearchPolicy_.finish(*request,E_UNEXPECTED);return E_UNEXPECTED;}
        if(!liveSearchOrigin_) {
            if(searchActive_&&searchScope_)liveSearchOrigin_.reset(ILCloneFull(searchScope_.get()));
            else if(searchBackground_&&historyIndex_>0)liveSearchOrigin_.reset(ILCloneFull(history_[historyIndex_-1].get()));
            else liveSearchOrigin_.reset(ILCloneFull(currentPidl_.get()));
        }
        if(!liveSearchOrigin_) {liveSearchPolicy_.finish(*request,E_OUTOFMEMORY);return E_OUTOFMEMORY;}
        liveSearchStatus_=startSearch(request->literal,searchRecursive_,{},L"",&*request);
    }
    const auto result=liveSearchStatus_;
    if(FAILED(result)) {
        liveSearchPolicy_.finish(*request,result);pendingLiveSearch_.reset();pendingLiveSearchTarget_.reset();scheduleLiveSearch();
    } else if(!navigating_&&currentPidl_&&pendingLiveSearchTarget_&&
              ILIsEqual(currentPidl_.get(),pendingLiveSearchTarget_.get()))completeLiveSearchNavigation(currentPidl_.get(),S_OK);
    return result;
}
HRESULT ExplorerApp::startSearch(const std::wstring& requested, bool recursive,
                                std::optional<size_t> category, const std::wstring& filter,
                                const LiveSearchRequest* liveRequest) {
    LiveSearchDispatchScope factoryDispatch(*this);
    if(!liveRequest)cancelLiveSearch();
    const auto directRevision=searchInteractionRevision_;
    auto query = trim(requested);
    if (query.empty() && !category) return S_FALSE;
    if(searchActive_&&folderView_) {
        const auto captured=captureActiveSearchPresentation();
        if(FAILED(captured))return captured;
    }
    const auto presentation=searchActive_?searchPresentation_:std::optional<SearchViewPresentation>{};
    auto base = query;
    std::array<std::wstring, 3> filters;
    if (searchActive_ && query == activeQuery_) { base = searchBase_; filters = searchFilters_; }
    if (category) {
        if (*category >= filters.size()) return E_INVALIDARG;
        filters[*category] = filter;
        query = base.empty() ? L"" : L"(" + base + L")";
        for (const auto& refinement : filters) if (!refinement.empty()) {
            if (!query.empty()) query += L" AND ";
            query += refinement;
        }
    }
    ComPtr<IShellItem> scope;
    auto hr = searchActive_ && searchScope_
        ? SHCreateItemFromIDList(searchScope_.get(), IID_PPV_ARGS(&scope)) : currentFolder(scope);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> results;
    ComPtr<IShellItemArray> scopes=searchActive_?searchScopes_:nullptr;
    if(!scopes&&library_.valid()&&!searchActive_)hr=library_.native()->GetFolders(LFF_ALLITEMS,IID_PPV_ARGS(&scopes));
    if(SUCCEEDED(hr)&&!scopes)hr=SHCreateShellItemArrayFromShellItem(scope.Get(),IID_PPV_ARGS(&scopes));
    if(FAILED(hr))return hr;
    auto rules=searchActive_?searchScopeRules_:std::vector<SearchScopeRule>{};
    if(rules.empty()) {
        DWORD count=0;hr=scopes->GetCount(&count);if(FAILED(hr))return hr;
        for(DWORD index=0;index<count;++index) {ComPtr<IShellItem> item;hr=scopes->GetItemAt(index,&item);if(FAILED(hr))return hr;rules.push_back({std::move(item),recursive,false});}
    }
    hr = createSearchFolderForScopeRules(query,rules,&results);
    if (FAILED(hr)) return hr;
    PIDLIST_ABSOLUTE raw = nullptr;
    hr = SHGetIDListFromObject(results.Get(), &raw);
    if (FAILED(hr)) return hr;
    Pidl location(raw);
    raw = nullptr;
    hr = SHGetIDListFromObject(scope.Get(), &raw);
    if (FAILED(hr)) return hr;
    if(!liveRequest&&headless_&&headlessSearchFactoryReentryProbe_) {
        auto probe=std::move(headlessSearchFactoryReentryProbe_);
        headlessSearchFactoryReentryProbe_={};probe();
    }
    if(liveRequest&&!liveSearchPolicy_.current(*liveRequest)) {CoTaskMemFree(raw);return S_FALSE;}
    if(!liveRequest&&searchInteractionRevision_!=directRevision) {CoTaskMemFree(raw);return S_FALSE;}
    Pidl directTarget;
    if(!liveRequest) {
        directTarget.reset(ILCloneFull(location.get()));
        if(!directTarget) {CoTaskMemFree(raw);return E_OUTOFMEMORY;}
    }
    searchLocations_.push_back({std::move(location), Pidl(raw), query, recursive, base, filters,scopes,std::move(rules),presentation,!liveRequest,false});
    if (searchLocations_.size() > 100) searchLocations_.erase(searchLocations_.begin());
    if(liveRequest) {
        pendingLiveSearch_=*liveRequest;
        pendingLiveSearchTarget_.reset(ILCloneFull(searchLocations_.back().location.get()));
        if(!pendingLiveSearchTarget_) {pendingLiveSearch_.reset();return E_OUTOFMEMORY;}
    } else {
        pendingDirectSearchTarget_=std::move(directTarget);
        pendingDirectSearchRevision_=directRevision;
    }
    hr = browser_->BrowseToObject(results.Get(), SBSP_ABSOLUTE);
    if(FAILED(hr)&&!liveRequest&&pendingDirectSearchRevision_==directRevision)pendingDirectSearchTarget_.reset();
    if(SUCCEEDED(hr)&&!liveRequest&&!navigating_&&currentPidl_&&!searchLocations_.empty()&&
       ILIsEqual(currentPidl_.get(),searchLocations_.back().location.get())&&!searchLocations_.back().remembered) {
        searchLocations_.back().remembered=true;rememberQuery(query);
        if(searchInteractionRevision_==directRevision)setSearchText(query);
        else if(pendingDirectSearchRevision_==directRevision)pendingDirectSearchTarget_.reset();
    }
    return hr;
}

void ExplorerApp::updateBreadcrumbs() {
    while (SendMessageW(breadcrumbs_, TB_BUTTONCOUNT, 0, 0)) SendMessageW(breadcrumbs_, TB_DELETEBUTTON, 0, 0);
    breadcrumbsPidls_.clear();
    if (!currentPidl_) return;
    SHFILEINFOW currentIcon{};
    const auto images=ImageList_Create(px(16),px(16),ILC_COLOR32|ILC_MASK,1,0);
    if(images) {
        if(SHGetFileInfoW(reinterpret_cast<LPCWSTR>(currentPidl_.get()),0,&currentIcon,sizeof(currentIcon),
            SHGFI_PIDL|SHGFI_ICON|SHGFI_SMALLICON)&&currentIcon.hIcon) {
            ImageList_AddIcon(images,currentIcon.hIcon);DestroyIcon(currentIcon.hIcon);
        } else if(folderIcon_)ImageList_AddIcon(images,folderIcon_);
        SendMessageW(breadcrumbs_,TB_SETIMAGELIST,0,reinterpret_cast<LPARAM>(images));
        if(breadcrumbImages_)ImageList_Destroy(breadcrumbImages_);
        breadcrumbImages_=images;
    }
    Pidl cursor(ILCloneFull(currentPidl_.get()));
    for (;;) {
        breadcrumbsPidls_.emplace_back(ILCloneFull(cursor.get()));
        if (ILIsEmpty(cursor.get()) || !ILRemoveLastID(cursor.get())) break;
    }
    std::reverse(breadcrumbsPidls_.begin(), breadcrumbsPidls_.end());
    // Keep the leaf and nearest ancestors accessible in narrow windows.
    if (breadcrumbsPidls_.size() > 7) breadcrumbsPidls_.erase(breadcrumbsPidls_.begin() + 1, breadcrumbsPidls_.end() - 5);
    for (size_t i = 0; i < breadcrumbsPidls_.size(); ++i) {
        auto label = pidlName(breadcrumbsPidls_[i].get(), SIGDN_NORMALDISPLAY);
        if (label.empty()) label = L"Desktop";

        TBBUTTON button{}; button.iBitmap = i==0?0:I_IMAGENONE;
        button.idCommand = BreadcrumbFirst + static_cast<int>(i);
        // MIXEDBUTTONS honors explicit widths only when SHOWTEXT is present.
        // The renderer uses dwData to retain Explorer's icon-only root while
        // the native provider keeps its accessible label and exact hit box.
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_DROPDOWN | BTNS_SHOWTEXT;
        button.dwData=i==0&&breadcrumbsPidls_.size()>1?1:0;
        button.iString = reinterpret_cast<INT_PTR>(label.c_str());
        SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button));
        TBBUTTONINFOW sizing{sizeof(sizing)};sizing.dwMask=TBIF_SIZE;
        if(i==0&&breadcrumbsPidls_.size()>1)sizing.cx=static_cast<WORD>(px(34));
        else {const auto dc=GetDC(breadcrumbs_);const auto old=dc&&font_?SelectObject(dc,font_):nullptr;SIZE text{};
            if(dc)GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&text);
            if(old)SelectObject(dc,old);if(dc)ReleaseDC(breadcrumbs_,dc);
            sizing.cx=static_cast<WORD>(std::min(px(220),static_cast<int>(text.cx)+px(i==0?42:24)));}
        SendMessageW(breadcrumbs_,TB_SETBUTTONINFOW,button.idCommand,reinterpret_cast<LPARAM>(&sizing));
    }
    TBBUTTON spacer{};spacer.idCommand=9021;spacer.fsStyle=BTNS_SEP;
    SendMessageW(breadcrumbs_,TB_ADDBUTTONS,1,reinterpret_cast<LPARAM>(&spacer));
    TBBUTTON edit{}; edit.iBitmap = I_IMAGENONE; edit.idCommand = AddressList;
    edit.fsState = TBSTATE_ENABLED; edit.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
    edit.iString = reinterpret_cast<INT_PTR>(L"Recent locations");
    SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&edit));
    TBBUTTONINFOW addressSize{sizeof(addressSize)};addressSize.dwMask=TBIF_SIZE;addressSize.cx=static_cast<WORD>(px(18));
    SendMessageW(breadcrumbs_,TB_SETBUTTONINFOW,AddressList,reinterpret_cast<LPARAM>(&addressSize));
    RECT bounds{}; GetClientRect(breadcrumbs_, &bounds); fitBreadcrumbs(bounds.right);
}
void ExplorerApp::editAddress() {
    addressEditing_ = true;
    SetWindowTextW(address_, currentLocation_.c_str());
    ShowWindow(breadcrumbs_, SW_HIDE); ShowWindow(address_, SW_SHOW);
    SetFocus(address_); SendMessageW(address_, EM_SETSEL, 0, -1);
}
void ExplorerApp::finishAddress(bool navigateNow) {
    auto value = textOf(address_);
    addressEditing_ = false;
    ShowWindow(address_, SW_HIDE); ShowWindow(breadcrumbs_, SW_SHOW);
    if (navigateNow) showError(navigate(value,true), L"Open location");
    if (view_) view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
}
HRESULT ExplorerApp::selection(ComPtr<IShellItemArray>& out, bool folderIfEmpty) {
    out.Reset();
    return folderView_ ? folderView_->GetSelection(folderIfEmpty, &out) : E_UNEXPECTED;
}
HRESULT ExplorerApp::currentFolder(ComPtr<IShellItem>& out) {
    out.Reset();
    return currentPidl_ ? SHCreateItemFromIDList(currentPidl_.get(), IID_PPV_ARGS(&out)) : E_UNEXPECTED;
}
void ExplorerApp::updateCommands() {
    if(commandRefreshActive_) {deferCommandRefresh();return;}
    CommandRefreshScope scope(*this);
    if(closing_)return;
    if (!nav_) return;
    const auto before=commandTimings_;
    const auto started=commandTimingNow();
    const auto previousMode = preferences_.view;
    FOLDERVIEWMODE actualMode=FVM_AUTO; int actualSize=0;
    if (folderView_ && SUCCEEDED(folderView_->GetViewModeAndIconSize(&actualMode,&actualSize))) {
        switch (actualMode) {
        case FVM_ICON: case FVM_THUMBNAIL: preferences_.view=actualSize>=192?ViewMode::ExtraLargeIcons:actualSize>=64?ViewMode::LargeIcons:ViewMode::MediumIcons; break;
        case FVM_SMALLICON: preferences_.view=ViewMode::SmallIcons; break;
        case FVM_LIST: preferences_.view=ViewMode::List; break;
        case FVM_DETAILS: preferences_.view=ViewMode::Details; break;
        case FVM_TILE: preferences_.view=ViewMode::Tiles; break;
        case FVM_CONTENT: preferences_.view=ViewMode::Content; break;
        default: break;
        }
    }
    if (preferences_.view != previousMode) namespaceDirty_ = true;
    SORTCOLUMN sorted{};
    if (folderView_ && SUCCEEDED(folderView_->GetSortColumns(&sorted, 1))) ascending_ = sorted.direction == SORT_ASCENDING;
    const auto clipboardSequence = GetClipboardSequenceNumber();
    if (clipboardSequence != clipboardSequence_) {
        clipboardSequence_ = clipboardSequence;
        clipboardFiles_ = IsClipboardFormatAvailable(CF_HDROP) ||
            IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
        namespaceDirty_ = true;
    }
    SendMessageW(nav_, TB_ENABLEBUTTON, Back, MAKELONG(historyIndex_ > 0, 0));
    SendMessageW(nav_, TB_ENABLEBUTTON, Forward, MAKELONG(historyIndex_ >= 0 && historyIndex_ + 1 < static_cast<int>(history_.size()), 0));
    SendMessageW(nav_, TB_ENABLEBUTTON, Up, MAKELONG(currentPidl_ && !ILIsEmpty(currentPidl_.get()), 0));
    SHELLSTATE shellSettings{};
    SHGetSetSettings(&shellSettings, SSF_SHOWEXTENSIONS, FALSE);
    preferences_.showExtensions = shellSettings.fShowExtensions;
    const auto viewCompleted=commandTimingNow();
    if(headless_)commandTimings_.viewReadbackMs+=viewCompleted-started;
    if (selectionStateDirty_) {
        selectionStateDirty_=false;
        const auto selectionStarted=commandTimingNow();
        ComPtr<IShellItemArray> selected;
        selectionCount_ = 0; selectionAttributes_ = 0;
        const bool selectionAvailable=SUCCEEDED(selection(selected))&&selected;
        if (selectionAvailable) {
            selected->GetCount(&selectionCount_);
            if (selectionCount_&&FAILED(selected->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),
                SFGAO_FILESYSTEM | SFGAO_HIDDEN | SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_HASPROPSHEET | SFGAO_FOLDER | SFGAO_LINK,
                &selectionAttributes_)))selectionAttributes_=0;
        }
        const auto attributesCompleted=commandTimingNow();
        if(headless_)commandTimings_.selectionCountAttributesMs+=attributesCompleted-selectionStarted;
        if(selectionAvailable)selectionStatus(selected.Get(), &selectionStatus_);else selectionStatus_ = {};
        const auto statusCompleted=commandTimingNow();
        if(headless_)commandTimings_.selectionStatusMs+=statusCompleted-attributesCompleted;
        selectionFilesystem_ = (selectionAttributes_ & SFGAO_FILESYSTEM) != 0;
        selectionHidden_ = (selectionAttributes_ & SFGAO_HIDDEN) != 0;
        SharePayload payload;
        selectionShareable_ = headless_ && selectionCount_ && selectionCount_ <= maximumShareFiles &&
            SUCCEEDED(makeSharePayload(selected.Get(), L"Share files", payload));
        namespaceDirty_ = true;
        if(headless_)commandTimings_.selectionHostEligibilityMs+=commandTimingNow()-statusCompleted;
    }
    if (namespaceDirty_ && !navigating_) updateContextTabsImpl();
    const auto ribbonStarted=commandTimingNow();
    bool minimized = false;
    if (SUCCEEDED(ribbon_.minimized(minimized))) preferences_.ribbonCollapsed = minimized;
    SendMessageW(statusView_, TB_CHECKBUTTON, ViewFirst + 5, MAKELONG(preferences_.view == ViewMode::Details, 0));
    SendMessageW(statusView_, TB_CHECKBUTTON, ViewFirst + 1, MAKELONG(preferences_.view == ViewMode::LargeIcons, 0));
    ribbon_.invalidateState();
    if(headless_) {
        const auto completed=commandTimingNow();
        commandTimings_.ribbonInvalidationMs+=completed-ribbonStarted;
        ++commandTimings_.updateCount;commandTimings_.totalMs+=completed-started;
        lastCommandTimings_=commandTimings_;
        lastCommandTimings_.updateCount-=before.updateCount;
        lastCommandTimings_.totalMs-=before.totalMs;
        lastCommandTimings_.viewReadbackMs-=before.viewReadbackMs;
        lastCommandTimings_.selectionCountAttributesMs-=before.selectionCountAttributesMs;
        lastCommandTimings_.selectionStatusMs-=before.selectionStatusMs;
        lastCommandTimings_.selectionHostEligibilityMs-=before.selectionHostEligibilityMs;
        lastCommandTimings_.selectionKindsMs-=before.selectionKindsMs;
        lastCommandTimings_.namespacePreparationMs-=before.namespacePreparationMs;
        lastCommandTimings_.providerCatalogMs-=before.providerCatalogMs;
        lastCommandTimings_.stateTaskSchedulingMs-=before.stateTaskSchedulingMs;
        lastCommandTimings_.contextMs-=before.contextMs;
        lastCommandTimings_.ribbonInvalidationMs-=before.ribbonInvalidationMs;
    }
}
void ExplorerApp::setStatus(const std::wstring& text) {
    if (status_) SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
void ExplorerApp::updateStatus() {
    if (archiveTask_.valid()) { setStatus(archiveAction_ + L"…"); return; }
    if (navigating_ || !folderView_) return;
    if (selectionStateDirty_) updateCommands();
    int items = 0;
    folderView_->ItemCount(SVGIO_ALLVIEW, &items);
    setStatus(statusText(static_cast<unsigned>(std::max(0, items)), selectionStatus_));
}
HRESULT ExplorerApp::browseHistory(int offset) {
    auto index = historyIndex_ + offset;
    if (index < 0 || index >= static_cast<int>(history_.size())) return S_FALSE;
    pendingHistory_ = index;
    const auto hr = browser_->BrowseToIDList(history_[index].get(), SBSP_ABSOLUTE);
    if (FAILED(hr)) pendingHistory_ = -1;
    return hr;
}
HRESULT ExplorerApp::setView(ViewMode mode) {
    if (!folderView_) return E_UNEXPECTED;
    FOLDERVIEWMODE native = FVM_ICON;
    int size = 48;
    switch (mode) {
    case ViewMode::ExtraLargeIcons: size = 256; break;
    case ViewMode::LargeIcons: size = 96; break;
    case ViewMode::MediumIcons: size = 48; break;
    case ViewMode::SmallIcons: native = FVM_SMALLICON; size = 16; break;
    case ViewMode::List: native = FVM_LIST; size = 16; break;
    case ViewMode::Details: native = FVM_DETAILS; size = 16; break;
    case ViewMode::Tiles: native = FVM_TILE; size = 48; break;
    case ViewMode::Content: native = FVM_CONTENT; size = 32; break;
    }
    const auto hr = folderView_->SetViewModeAndIconSize(native, size);
    if (SUCCEEDED(hr)) { preferences_.view = mode; namespaceDirty_ = true; updateCommands(); }
    return hr;
}
HRESULT ExplorerApp::setSort(const PROPERTYKEY& key) {
    if (!folderView_) return E_UNEXPECTED;
    SORTCOLUMN column{key, ascending_ ? SORT_ASCENDING : SORT_DESCENDING};
    return folderView_->SetSortColumns(&column, 1);
}
HRESULT ExplorerApp::setGroup(const PROPERTYKEY& key) {
    return folderView_ ? folderView_->SetGroupBy(key, ascending_) : E_UNEXPECTED;
}
HRESULT ExplorerApp::nativeVerb(const wchar_t* verb, bool folderIfEmpty) {
    if (headless_) return E_ACCESSDENIED;
    ComPtr<IShellItemArray> items;
    auto hr = selection(items, folderIfEmpty);
    if(FAILED(hr))return hr;
    if(verb&&_wcsicmp(verb,L"open")==0&&browser_) {
        DWORD count=0;ComPtr<IShellItem> folder;SFGAOF attributes=0;
        if(items&&SUCCEEDED(items->GetCount(&count))&&count==1&&SUCCEEDED(items->GetItemAt(0,&folder))&&
           SUCCEEDED(folder->GetAttributes(SFGAO_FOLDER|SFGAO_LINK,&attributes))&&
           (attributes&SFGAO_FOLDER)&&!(attributes&SFGAO_LINK)) {
            if(newWindowMode_||(GetKeyState(VK_CONTROL)&0x8000))
                return openNewWindow(itemName(folder.Get(),SIGDN_DESKTOPABSOLUTEPARSING));
            return browser_->BrowseToObject(folder.Get(),SBSP_ABSOLUTE);
        }
    }
    return ShellOperations::invoke(window_,items.Get(),verb,view_.Get());
}
void ExplorerApp::refreshAddressHistoryPolicy() {
    if(headless_)return;
    const bool allowed=typedAddressHistoryAllowed();
    if(typedAddressHistoryLoaded_&&allowed==typedAddressHistoryAllowed_)return;
    typedAddressHistoryLoaded_=true;typedAddressHistoryAllowed_=allowed;
    typedAddresses_.clear();pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
    if(!allowed)return;
    addressHistoryStatus_=loadAddressHistory(addressHistoryPath(),&typedAddresses_);
    std::vector<std::wstring> imported;
    const auto nativeRead=loadWindowsTypedAddresses(&imported);
    if(SUCCEEDED(nativeRead)) {
        const auto merge=mergeTypedAddressHistory(typedAddresses_,imported);
        if(FAILED(merge))addressHistoryStatus_=merge;
    } else if(SUCCEEDED(addressHistoryStatus_))addressHistoryStatus_=nativeRead;
}
void ExplorerApp::rememberAddressNavigation(PCIDLIST_ABSOLUTE location) {
    if(pendingTypedAddress_.empty()||!pendingTypedAddressTarget_)return;
    const bool completed=location&&ILIsEqual(location,pendingTypedAddressTarget_.get());
    auto typed=std::move(pendingTypedAddress_);
    pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
    if(completed&&typedAddressHistoryAllowed_&&rememberTypedAddress(typedAddresses_,typed)&&!headless_)
        addressHistoryStatus_=saveAddressHistory(addressHistoryPath(),typedAddresses_);
}
HMENU ExplorerApp::createTypedAddressMenu() const {
    const auto menu=CreatePopupMenu();
    if(!menu)return nullptr;
    for(size_t index=0;index<typedAddresses_.size()&&index<maximumTypedAddresses;++index) {
        std::wstring label;label.reserve(typedAddresses_[index].size());
        for(const auto ch:typedAddresses_[index]){label+=ch;if(ch==L'&')label+=ch;}
        if(!AppendMenuW(menu,MF_STRING,TypedAddressFirst+static_cast<UINT>(index),label.c_str())) {
            DestroyMenu(menu);return nullptr;
        }
    }
    return menu;
}
void ExplorerApp::updateRibbonCollapseButton() {
    if(!ribbonCollapse_||!window_||!ribbon_.valid())return;
    bool minimized=false;
    if(FAILED(ribbon_.minimized(minimized)))return;
    const wchar_t* label=minimized?L"Expand the Ribbon":L"Minimize the Ribbon";
    if(textOf(ribbonCollapse_)!=label)SetWindowTextW(ribbonCollapse_,label);
    const auto tip=std::wstring(label)+L" (Ctrl+F1)";
    if(tip!=ribbonCollapseTip_) {
        ribbonCollapseTip_=tip;
        if(ribbonCollapseTooltip_) {
            TOOLINFOW tool{sizeof(tool)};tool.hwnd=window_;tool.uId=reinterpret_cast<UINT_PTR>(ribbonCollapse_);
            tool.lpszText=ribbonCollapseTip_.data();
            SendMessageW(ribbonCollapseTooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
        }
    }
    HWND bar=nullptr;
    EnumChildWindows(window_,[](HWND child,LPARAM context)->BOOL {
        wchar_t name[64]{};GetClassNameW(child,name,static_cast<int>(std::size(name)));
        if(wcscmp(name,L"UIRibbonCommandBar")!=0)return TRUE;
        *reinterpret_cast<HWND*>(context)=child;return FALSE;
    },reinterpret_cast<LPARAM>(&bar));
    RECT tabs{};
    if(bar&&GetWindowRect(bar,&tabs)) {
        MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&tabs),2);
        SetWindowPos(ribbonCollapse_,HWND_TOP,std::max(0,static_cast<int>(tabs.right)-px(44)),tabs.top,
            px(22),px(24),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(ribbonCollapse_,nullptr,FALSE);
    }
}
HRESULT ExplorerApp::chooseDestination(bool move) {
    if (headless_) return E_ACCESSDENIED;
    ComPtr<IShellItemArray> items;
    auto hr = selection(items);
    if (FAILED(hr)) return hr;
    ComPtr<IFileOpenDialog> dialog;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return hr;
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(move ? L"Move selected items to" : L"Copy selected items to");
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(currentFolder(folder))) dialog->SetFolder(folder.Get());
    hr = dialog->Show(window_);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> destination;
    hr = dialog->GetResult(&destination);
    return SUCCEEDED(hr) ? ShellOperations::copyOrMove(window_, items.Get(), destination.Get(), move) : hr;
}
HRESULT ExplorerApp::newFolder() {
    if (headless_) return E_ACCESSDENIED;
    if (navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder);
    if (FAILED(hr)) return hr;
    if (!isPhysicalDirectory(folder.Get())) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    const auto base = itemName(folder.Get(), SIGDN_FILESYSPATH);
    if (base.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::wstring name = L"New folder";
    std::error_code error;
    unsigned i = 2;
    while (std::filesystem::exists(std::filesystem::path(base) / name, error)) {
        if (i > 30000) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
        name = L"New folder (" + std::to_wstring(i++) + L")";
    }
    if (error) return HRESULT_FROM_WIN32(error.value());
    hr = ShellOperations::newFolder(window_, folder.Get(), name);
    if (SUCCEEDED(hr)) {
        PIDLIST_ABSOLUTE raw = nullptr;
        auto path = (std::filesystem::path(base) / name).wstring();
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr))) {
            Pidl pidl(raw);
            if (view_) view_->SelectItem(ILFindLastID(pidl.get()), SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_EDIT | SVSI_ENSUREVISIBLE);
        }
    }
    return hr;
}
HRESULT ExplorerApp::newText() {
    if (headless_) return E_ACCESSDENIED;
    if (navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder);
    if (FAILED(hr)) return hr;
    if (!isPhysicalDirectory(folder.Get())) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    auto base = itemName(folder.Get(), SIGDN_FILESYSPATH);
    if (base.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::wstring name = L"New Text Document.txt";
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned i = 1; i < 30000; ++i) {
        auto path = (std::filesystem::path(base) / name).wstring();
        file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, path.c_str(), nullptr);
            return S_OK;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
        name = L"New Text Document (" + std::to_wstring(i + 1) + L").txt";
    }
    return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
}
HRESULT ExplorerApp::showProperties(const wchar_t* page) {
    if (headless_) return E_ACCESSDENIED;
    if (!page) return nativeVerb(L"properties", true);
    ComPtr<IShellItemArray> items;
    auto hr = selection(items, true);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> item;
    hr = items->GetItemAt(0, &item);
    if (FAILED(hr)) return hr;
    auto path = itemName(item.Get(), SIGDN_FILESYSPATH);
    if (path.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    return SHObjectProperties(window_, SHOP_FILEPATH, path.c_str(), page) ? S_OK : E_FAIL;
}
HRESULT ExplorerApp::archive(bool extract) {
    if (headless_) return E_ACCESSDENIED;
    if (archiveTask_.valid()) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ComPtr<IShellItemArray> items;
    auto hr = selection(items); if (FAILED(hr) || !items) return FAILED(hr) ? hr : E_INVALIDARG;
    DWORD count = 0; items->GetCount(&count);
    if (!count || (extract && count != 1)) return E_INVALIDARG;
    std::vector<std::filesystem::path> sources;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        hr = items->GetItemAt(i, &item); if (FAILED(hr)) return hr;
        auto path = itemName(item.Get(), SIGDN_FILESYSPATH);
        if (path.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        sources.emplace_back(path);
    }
    std::filesystem::path output;
    if (extract) {
        ComPtr<IFileOpenDialog> dialog;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) return hr;
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Extract to a new folder inside");
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(currentFolder(folder))) dialog->SetFolder(folder.Get());
        hr = dialog->Show(window_); if (FAILED(hr)) return hr;
        ComPtr<IShellItem> destination; hr = dialog->GetResult(&destination); if (FAILED(hr)) return hr;
        hr = unusedPath(itemName(destination.Get(), SIGDN_FILESYSPATH), sources.front().stem().wstring(), L"", output);
        if (FAILED(hr)) return hr;
    } else {
        ComPtr<IFileSaveDialog> dialog;
        hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) return hr;
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
        dialog->SetTitle(L"Create ZIP archive (choose a new file)");
        COMDLG_FILTERSPEC filter{L"Compressed (zipped) folder", L"*.zip"};
        dialog->SetFileTypes(1, &filter); dialog->SetDefaultExtension(L"zip");
        auto name = sources.front().filename().wstring();
        if (name.empty()) name = L"Archive";
        dialog->SetFileName((name + L".zip").c_str());
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(currentFolder(folder))) dialog->SetFolder(folder.Get());
        hr = dialog->Show(window_); if (FAILED(hr)) return hr;
        ComPtr<IShellItem> destination; hr = dialog->GetResult(&destination); if (FAILED(hr)) return hr;
        output = itemName(destination.Get(), SIGDN_FILESYSPATH);
    }
    archiveAction_ = extract ? L"Extracting archive" : L"Creating ZIP archive";
    archiveTask_ = std::async(std::launch::async, [sources = std::move(sources), output, extract] {
        try {
            return extract ? ExtraOperations::extractZip(sources.front(), output) : ExtraOperations::createZip(sources, output);
        } catch (...) { return E_FAIL; }
    });
    updateCommands(); updateStatus();
    return S_OK;
}
HRESULT ExplorerApp::makeShortcut(bool fromClipboard) {
    if (headless_) return E_ACCESSDENIED;
    if (navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder); if (FAILED(hr)) return hr;
    if (!isPhysicalDirectory(folder.Get())) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    auto destination = itemName(folder.Get(), SIGDN_FILESYSPATH);
    if (destination.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::vector<std::filesystem::path> targets;
    if (fromClipboard) {
        ComPtr<IDataObject> clipboard;
        hr = OleGetClipboard(&clipboard); if (FAILED(hr)) return hr;
        ComPtr<IShellItemArray> items;
        hr = SHCreateShellItemArrayFromDataObject(clipboard.Get(), IID_PPV_ARGS(&items));
        if (FAILED(hr)) return hr;
        DWORD count = 0; items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item; hr = items->GetItemAt(i, &item); if (FAILED(hr)) return hr;
            auto path = itemName(item.Get(), SIGDN_FILESYSPATH);
            if (path.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            targets.emplace_back(path);
        }
    } else {
        ComPtr<IFileOpenDialog> dialog;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) return hr;
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose a file for the new shortcut");
        hr = dialog->Show(window_); if (FAILED(hr)) return hr;
        ComPtr<IShellItem> item; hr = dialog->GetResult(&item); if (FAILED(hr)) return hr;
        targets.emplace_back(itemName(item.Get(), SIGDN_FILESYSPATH));
    }
    if (targets.empty()) return E_INVALIDARG;
    for (const auto& target : targets) {
        std::filesystem::path output;
        auto name = target.filename().wstring();
        if (name.empty()) name = L"Folder";
        hr = unusedPath(destination, name + L" - Shortcut", L".lnk", output); if (FAILED(hr)) return hr;
        hr = ExtraOperations::createShortcut(target, output); if (FAILED(hr)) return hr;
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, output.c_str(), nullptr);
    }
    return S_OK;
}

HRESULT ExplorerApp::execute(UINT command) {
    if(commandRefreshActive_) {deferCommandRefresh();return HRESULT_FROM_WIN32(ERROR_RETRY);}
    if(closing_)return E_ABORT;
    if(command==ExpandAncestors) {
        const auto persistent=expandCurrent_;
        expandCurrent_=true;
        const auto hr=applyNavigationOptions();
        expandCurrent_=persistent;
        return hr;
    }
    if(headless_&&command==OpenFileLocation)return openFileLocation();
    if(headless_&&command==NewFolder)return newFolder();
    if(command==NewFolder&&librariesRoot_)return newLibrary();
    if (const auto binding = appCommandBinding(command); binding && binding->route != AppCommandRoute::Host)
        return executeRibbon(command);
    if (command == Terminal || command == NewShortcut || command == NewText || command == Zip ||
        command == Extract || command == Security || command == LibraryLocations) return executeRibbon(command);
    if (command >= BreadcrumbFirst && command - BreadcrumbFirst < breadcrumbsPidls_.size())
        return browser_->BrowseToIDList(breadcrumbsPidls_[command - BreadcrumbFirst].get(), SBSP_ABSOLUTE);
    if (command >= ViewFirst && command <= ViewLast) return setView(static_cast<ViewMode>(command - ViewFirst));
    ComPtr<IShellItemArray> items;
    ComPtr<IShellItem> folder;
    switch (command) {
    case Back: return browseHistory(-1);
    case Forward: return browseHistory(1);
    case Up: return browser_->BrowseToIDList(nullptr, SBSP_PARENT);
    case Refresh: return view_ ? view_->Refresh() : E_UNEXPECTED;
    case Address: editAddress(); return S_OK;
    case AddressList: popup(AddressList,breadcrumbs_);return headless_?E_ACCESSDENIED:S_OK;
    case FocusNext: return cycleFocus(false);
    case FocusPrevious: return cycleFocus(true);
    case Fullscreen: return toggleFullscreen();
    case FocusSearch: SetFocus(search_); SendMessageW(search_, EM_SETSEL, 0, -1); return S_OK;
    case Search: {
        const auto submitted=liveSearchPolicy_.submit(textOf(search_),GetTickCount64());
        return FAILED(submitted)?submitted:processLiveSearch();
    }
    case CloseSearch:
        if(liveSearchOrigin_) {
            liveSearchPolicy_.escape(GetTickCount64());setSearchText(L"",false);return processLiveSearch();
        }
        if (!searchActive_ || !searchScope_) return S_FALSE;
        setSearchText(L"");
        return browser_->BrowseToIDList(searchScope_.get(), SBSP_ABSOLUTE);
    case SearchSubfolders: case SearchCurrent: {
        if(!searchActive_)return S_FALSE;
        const auto previous=searchScopeRules_;
        for(auto& rule:searchScopeRules_)if(!rule.excluded)rule.recursive=command==SearchSubfolders;
        const auto hr=startSearch(activeQuery_,command==SearchSubfolders);
        if(FAILED(hr))searchScopeRules_=previous;
        return hr;
    }
    case SaveSearch: return saveSearch();
    case OpenFileLocation: return openFileLocation();
    case NewItems: return newItemMenu();
    case NewLibrary: return newLibrary();
    case IncludeLibraryFolder: return includeLibraryFolder();
    case QuickAccessMenu: return S_FALSE;
    case QuickAccessPlacement:
        { bool below = false; auto hr = ribbon_.quickAccessBelow(below);
          if (FAILED(hr)) return hr; hr = ribbon_.setQuickAccessBelow(!below); layout(); return hr; }
    case QuickAccessReset: quickAccessModel_.reset(); rebuildQuickAccess(); rebuildRibbon(); return S_OK;
    case QuickAccess: return navigate(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}");
    case ThisPC: return navigate(L"shell:MyComputerFolder");
    case Desktop: return navigate(L"shell:Desktop");
    case Documents: return navigate(L"shell:Personal");
    case Downloads: return navigate(L"shell:Downloads");
    case Pictures: return navigate(L"shell:My Pictures");
    case Music: return navigate(L"shell:My Music");
    case Videos: return navigate(L"shell:My Video");
    case Network: return navigate(L"shell:NetworkPlacesFolder");
    case RecycleBin: return navigate(L"shell:RecycleBinFolder");
    case Libraries: return navigate(L"shell:Libraries");
    case FileMenu: case HistoryMenu: case ViewMenu: case SortMenu: case GroupMenu:
    case ColumnsMenu: case RecentSearches: case SearchKindMenu: case SearchDateMenu: case SearchSizeMenu:
    case LibraryLocations: case LibraryDefault: case LibraryOptimize:
        popup(command); return S_OK;
    case SizeColumns: return sizeColumns();
    case Close: PostMessageW(window_, WM_CLOSE, 0, 0); return S_OK;
    case NewWindow: {
        return openNewWindow(currentLocation_);
    }
    case Copy: case Cut: case CopyPath: {
        if (headless_) return E_ACCESSDENIED;
        const auto hr = selection(items); if (FAILED(hr)) return hr;
        return command == CopyPath ? ShellOperations::copyPaths(window_, items.Get()) : ShellOperations::copyToClipboard(window_, items.Get(), command == Cut);
    }
    case Paste: {
        if (headless_) return E_ACCESSDENIED;
        const auto hr = currentFolder(folder);
        return SUCCEEDED(hr) ? ShellOperations::paste(window_, folder.Get()) : hr;
    }
    case PasteShortcut: return makeShortcut(true);
    case HideSelected: {
        if (headless_) return E_ACCESSDENIED;
        auto hr = selection(items); if (FAILED(hr)) return hr;
        HRESULT rollback = S_OK;
        hr = ItemActions::toggleHidden(items.Get(), &rollback);
        if (FAILED(rollback)) showError(rollback, L"Restore hidden attributes");
        if (view_) view_->Refresh();
        return hr;
    }
    case CopyTo: return chooseDestination(false);
    case MoveTo: return chooseDestination(true);
    case Delete: case PermanentDelete: {
        if (headless_) return E_ACCESSDENIED;
        const auto hr = selection(items); if (FAILED(hr)) return hr;
        DWORD count = 0;
        if (!items) return E_INVALIDARG;
        const auto countResult = items->GetCount(&count);
        if (FAILED(countResult)) return countResult;
        if (!count) return E_INVALIDARG;
        if (command == PermanentDelete && MessageBoxW(window_, L"Permanently delete the selected items? They will not go to the Recycle Bin.", L"Delete permanently", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return S_FALSE;
        return ShellOperations::remove(window_, items.Get(), command == PermanentDelete);
    }
    case Rename: return folderView_ ? folderView_->DoRename() : E_UNEXPECTED;
    case NewFolder: return newFolder();
    case NewText: return newText();
    case NewShortcut: return makeShortcut(false);
    case Properties: return showProperties(nullptr);
    case Sharing: return shareFiles();
    case SharingProperties: return showProperties(L"Sharing");
    case Security: return showProperties(L"Security");
    case Open: return nativeVerb(L"open");
    case Edit: return nativeVerb(L"edit");
    case Print: return nativeVerb(L"print");
    case Pin: return nativeVerb(L"pintohome", true);
    case Undo: case Redo:
        if (headless_) return E_ACCESSDENIED;
        { auto hr = command == Undo ? ShellOperations::undo(window_) : ShellOperations::redo(window_);
          namespaceDirty_ = true; ribbon_.invalidateState(); return hr; }
    case Zip: return archive(false);
    case Extract: return archive(true);
    case SelectAll: case SelectNone: case Invert: {
        updateNamespace();
        CommandRefreshScope nativeCommand(*this);
        const auto action=command==SelectAll?SelectionAction::All:command==SelectNone?SelectionAction::None:SelectionAction::Invert;
        return changeShellSelection(folderView_.Get(),view_.Get(),action,&backgroundActions_,headless_);
    }
    case NavigationPane: preferences_.navigationPane = !preferences_.navigationPane; break;
    case PreviewPane: preferences_.previewPane = !preferences_.previewPane; preferences_.detailsPane = false; break;
    case DetailsPane: preferences_.detailsPane = !preferences_.detailsPane; preferences_.previewPane = false; break;
    case HiddenItems:
        preferences_.showHidden = !preferences_.showHidden;
        if(!headless_) {
            SHELLSTATE settings{};SHGetSetSettings(&settings,SSF_SHOWALLOBJECTS,FALSE);
            settings.fShowAllObjects=preferences_.showHidden;
            SHGetSetSettings(&settings,SSF_SHOWALLOBJECTS,TRUE);
            SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
        }
        break;
    case Extensions: {
        if (headless_) return E_ACCESSDENIED;
        SHELLSTATE state{}; SHGetSetSettings(&state, SSF_SHOWEXTENSIONS, FALSE);
        state.fShowExtensions = !state.fShowExtensions;
        SHGetSetSettings(&state, SSF_SHOWEXTENSIONS, TRUE);
        preferences_.showExtensions = state.fShowExtensions;
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        if (view_) view_->Refresh();
        rebuildRibbon(); return S_OK;
    }
    case Collapse: {
        bool minimized=false;auto hr=ribbon_.minimized(minimized);
        if(SUCCEEDED(hr))hr=ribbon_.setMinimized(!minimized);
        if(SUCCEEDED(hr)){preferences_.ribbonCollapsed=!minimized;layout();}
        return hr;
    }
    case Checkboxes:
        checkboxes_ = !checkboxes_; rebuildRibbon();
        return folderView_ ? folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, checkboxes_ ? FWF_CHECKSELECT : 0) : E_UNEXPECTED;
    case SortName: return setSort(PKEY_ItemNameDisplay);
    case SortDate: return setSort(PKEY_DateModified);
    case SortType: return setSort(PKEY_ItemTypeText);
    case SortSize: return setSort(PKEY_Size);
    case SortAscending: case SortDescending: {
        ascending_ = command == SortAscending;
        SORTCOLUMN column{};
        if (folderView_ && SUCCEEDED(folderView_->GetSortColumns(&column, 1))) return setSort(column.propkey);
        return setSort(PKEY_ItemNameDisplay);
    }
    case GroupNone: return setGroup(PKEY_Null);
    case GroupName: return setGroup(PKEY_ItemNameDisplay);
    case GroupDate: return setGroup(PKEY_DateModified);
    case GroupType: return setGroup(PKEY_ItemTypeText);
    case GroupSize: return setGroup(PKEY_Size);
    case FolderOptions: return headless_ ? E_ACCESSDENIED : shellExecute(window_, L"control.exe", L"folders");
    case FileHistory: {
        updateNamespace();CommandRefreshScope nativeCommand(*this);
        return namespaceActions_.invoke(NamespaceAction::FileHistory,headless_);
    }
    case MapDrive: case DisconnectDrive:
        if (headless_) return E_ACCESSDENIED;
        { auto result = command == MapDrive ? WNetConnectionDialog(window_, RESOURCETYPE_DISK) : WNetDisconnectDialog(window_, RESOURCETYPE_DISK);
          return result == NO_ERROR ? S_OK : result == static_cast<DWORD>(-1) ? S_FALSE : HRESULT_FROM_WIN32(result); }
    case Terminal: {
        if (headless_) return E_ACCESSDENIED;
        if (navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
        auto hr = currentFolder(folder); if (FAILED(hr)) return hr;
        if (!isPhysicalDirectory(folder.Get())) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        auto path = itemName(folder.Get(), SIGDN_FILESYSPATH);
        return path.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : shellExecute(window_, L"cmd.exe", nullptr, path.c_str());
    }
    default: return E_NOTIMPL;
    }
    rebuildRibbon();
    return recreateBrowser();
}

void ExplorerApp::popup(UINT command, HWND anchor) {
    if (headless_) return;
    if(commandRefreshActive_) {deferCommandRefresh();return;}
    if(command==AddressList) {
        const auto addresses=typedAddresses_;
        const auto menu=createTypedAddressMenu();
        if(!menu){showError(HRESULT_FROM_WIN32(GetLastError()),L"Recent locations");return;}
        RECT button{};
        if(!SendMessageW(breadcrumbs_,TB_GETRECT,AddressList,reinterpret_cast<LPARAM>(&button))) {
            DestroyMenu(menu);return;
        }
        MapWindowPoints(breadcrumbs_,nullptr,reinterpret_cast<POINT*>(&button),2);
        const auto selected=addresses.empty()?0:TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN|TPM_TOPALIGN,
            button.left,button.bottom,0,window_,nullptr);
        DestroyMenu(menu);
        if(selected>=TypedAddressFirst&&selected-TypedAddressFirst<addresses.size()) {
            addressEditing_=false;
            ShowWindow(address_,SW_HIDE);ShowWindow(breadcrumbs_,SW_SHOW);
            showError(navigate(addresses[selected-TypedAddressFirst],true),L"Open location");
            if(view_)view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
        }
        return;
    }
    if (command == RibbonOpenWith || command == SortMenu || command == GroupMenu || command == ColumnsMenu || command == LibraryDefault ||
        command == LibraryOptimize || command == RibbonLibraryOptimizeMenu) {
        updateNamespace();
        CommandRefreshScope nativeCommand(*this);
        NamespaceCommandPopup native;
        const auto binding=appCommandBinding(command);
        auto& actions=binding&&binding->scope==NamespaceMenuScope::Background?backgroundActions_:namespaceActions_;
        auto hr = queryAppCommandPopup(actions, command, commandContext(), &native);
        POINT point{};
        if (anchor) { RECT rect{}; GetWindowRect(anchor, &rect); point = {rect.left, rect.bottom}; }
        else GetCursorPos(&point);
        activeNamespaceMenu_=&actions;
        if (SUCCEEDED(hr)) hr = actions.invokeCommandStorePopup(native, false, point);
        activeNamespaceMenu_=nullptr;
        namespaceDirty_ = selectionStateDirty_ = true;
        if (SUCCEEDED(hr) && (command == LibraryDefault || command == LibraryOptimize || command == RibbonLibraryOptimizeMenu)) reloadLibrary();
        showError(hr, L"Command"); updateCommands(); return;
    }
    auto menu = CreatePopupMenu();
    auto add = [&](UINT id, const wchar_t* text, bool checked = false, bool disabled = false) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (disabled ? MF_GRAYED : 0), id, text);
    };
    auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    std::vector<PROPERTYKEY> columnKeys;
    std::vector<std::wstring> refinements;
    std::vector<LibraryFolder> libraryFolders;
    Pidl libraryTarget(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
    const auto libraryGeneration = navigationCount_;
    auto refine = [&](const wchar_t* label, const wchar_t* query) {
        add(28000 + static_cast<UINT>(refinements.size()), label);
        refinements.emplace_back(query);
    };
    if (command == LibraryLocations || command == LibraryDefault) {
        const auto hr = library_.folders(libraryFolders);
        if (FAILED(hr)) { DestroyMenu(menu); showError(hr, L"Read library locations"); return; }
        ComPtr<IShellItem> defaultFolder;
        library_.defaultSaveFolder(defaultFolder);
        for (size_t i = 0; i < libraryFolders.size(); ++i) {
            int order = 1;
            const bool current = defaultFolder && SUCCEEDED(libraryFolders[i].item->Compare(defaultFolder.Get(),
                SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL, &order)) && order == 0;
            const auto label = libraryFolders[i].path.empty() ? libraryFolders[i].displayName : libraryFolders[i].path.wstring();
            if (command == LibraryDefault) add(35000 + static_cast<UINT>(i), label.c_str(), current, !library_.writable());
            else {
                const auto location = CreatePopupMenu();
                AppendMenuW(location, MF_STRING, 34000 + i, L"Open location");
                AppendMenuW(location, MF_STRING | (current ? MF_CHECKED : 0) | (library_.writable() ? 0 : MF_GRAYED), 35000 + i, L"Set as save location");
                AppendMenuW(location, MF_STRING | (library_.writable() ? 0 : MF_GRAYED), 36000 + i, L"Remove from library");
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(location), label.c_str());
            }
        }
        if (libraryFolders.empty()) add(0, L"No included locations", false, true);
    } else if (command == LibraryOptimize) {
        GUID type{}; library_.folderType(type);
        const auto current = libraryKindForType(type);
        const auto kinds = libraryKinds();
        for (size_t i = 0; i < kinds.size(); ++i)
            add(37000 + static_cast<UINT>(i), kinds[i].label.data(), current == kinds[i].kind, !library_.writable());
    } else if (command == QuickAccessMenu) {
        const auto catalog = quickAccessCommands();
        const auto choices = CreatePopupMenu();
        for (size_t i = 0; i < catalog.size(); ++i)
            AppendMenuW(choices, MF_STRING | (quickAccessModel_.contains(catalog[i].command) ? MF_CHECKED : 0),
                30000 + i, catalog[i].label.data());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(choices), L"Add or remove commands");
        const auto& commands = quickAccessModel_.commands();
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto* metadata = quickAccessCommand(commands[i]);
            if (!metadata) continue;
            const auto order = CreatePopupMenu();
            AppendMenuW(order, MF_STRING | (i ? 0 : MF_GRAYED), 31000 + i, L"Move earlier");
            AppendMenuW(order, MF_STRING | (i + 1 < commands.size() ? 0 : MF_GRAYED), 32000 + i, L"Move later");
            AppendMenuW(order, MF_STRING, 33000 + i, L"Remove from toolbar");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(order), metadata->label.data());
        }
        separator();
        add(QuickAccessPlacement, quickAccessModel_.belowRibbon() ? L"Show above the ribbon" : L"Show below the ribbon");
        add(QuickAccessReset, L"Reset toolbar");
    } else if (command == FileMenu) {
        add(NewWindow, L"Open new window\tCtrl+N"); add(Terminal, L"Open command prompt here", false, !physicalDirectory_ || navigating_); separator();
        add(QuickAccess, L"Quick access"); add(ThisPC, L"This PC"); add(Desktop, L"Desktop");
        add(Documents, L"Documents"); add(Downloads, L"Downloads"); add(Pictures, L"Pictures");
        add(Music, L"Music"); add(Videos, L"Videos"); add(Libraries, L"Libraries");
        add(Network, L"Network"); add(RecycleBin, L"Recycle Bin"); separator();
        add(FolderOptions, L"Change folder and search options");
        add(NewLibrary, L"New library…");
        add(QuickAccessMenu, L"Customize Quick Access Toolbar");
        add(Collapse, L"Minimize the ribbon\tCtrl+F1", preferences_.ribbonCollapsed); separator();
        add(Fullscreen, L"Full screen\tF11", fullscreen_); separator();
        add(Close, L"Close\tAlt+F4");
    } else if (command == ViewMenu) {
        for (int i = 0; i < 8; ++i) add(ViewFirst + i, ViewNames[i], static_cast<int>(preferences_.view) == i);
    } else if (command == SortMenu || command == GroupMenu) {
        const bool group = command == GroupMenu;
        add(group ? GroupName : SortName, L"Name"); add(group ? GroupDate : SortDate, L"Date modified");
        add(group ? GroupType : SortType, L"Type"); add(group ? GroupSize : SortSize, L"Size");
        if (group) add(GroupNone, L"(None)");
        else { separator(); add(SortAscending, L"Ascending", ascending_); add(SortDescending, L"Descending", !ascending_); }
    } else if (command == HistoryMenu) {
        for (int i = static_cast<int>(history_.size()) - 1; i >= 0; --i) {
            auto name = pidlName(history_[i].get(), SIGDN_NORMALDISPLAY);
            add(4000 + i, name.c_str(), i == historyIndex_);
        }
    } else if (command == ColumnsMenu) {
        ComPtr<IColumnManager> columns;
        UINT count = 0;
        if (folderView_ && SUCCEEDED(folderView_.As(&columns)) && SUCCEEDED(columns->GetColumnCount(CM_ENUM_ALL, &count)) && count <= 1000) {
            columnKeys.resize(count);
            if (SUCCEEDED(columns->GetColumns(CM_ENUM_ALL, columnKeys.data(), count))) {
                for (UINT i = 0; i < count; ++i) {
                    CM_COLUMNINFO info{sizeof(info)}; info.dwMask = CM_MASK_NAME | CM_MASK_STATE;
                    if (SUCCEEDED(columns->GetColumnInfo(columnKeys[i], &info)) && *info.wszName)
                        add(26000 + i, info.wszName, (info.dwState & CM_STATE_VISIBLE) != 0,
                            IsEqualPropertyKey(columnKeys[i], PKEY_ItemNameDisplay));
                }
            }
        }
        separator(); add(SizeColumns, L"Size all columns to fit");
    } else if (command == RecentSearches) {
        for (size_t i = 0; i < recentSearches_.size(); ++i) add(27000 + static_cast<UINT>(i), recentSearches_[i].c_str());
    } else if (command == SearchKindMenu) {
        refine(L"Documents", L"System.Kind:=System.Kind#Document"); refine(L"Pictures", L"System.Kind:=System.Kind#Picture");
        refine(L"Music", L"System.Kind:=System.Kind#Music"); refine(L"Videos", L"System.Kind:=System.Kind#Video");
        refine(L"Folders", L"System.Kind:=System.Kind#Folder"); refine(L"Programs", L"System.Kind:=System.Kind#Program");
    } else if (command == SearchDateMenu) {
        refine(L"Today", L"System.DateModified:System.StructuredQueryType.DateTime#Today");
        refine(L"Yesterday", L"System.DateModified:System.StructuredQueryType.DateTime#Yesterday");
        refine(L"This week", L"System.DateModified:System.StructuredQueryType.DateTime#ThisWeek");
        refine(L"Last week", L"System.DateModified:System.StructuredQueryType.DateTime#LastWeek");
        refine(L"This month", L"System.DateModified:System.StructuredQueryType.DateTime#ThisMonth");
        refine(L"This year", L"System.DateModified:System.StructuredQueryType.DateTime#ThisYear");
    } else if (command == SearchSizeMenu) {
        refine(L"Empty (0 KB)", L"System.Size:System.Size#Empty");
        refine(L"Tiny (0–16 KB)", L"System.Size:System.Size#Tiny");
        refine(L"Small (16 KB–1 MB)", L"System.Size:System.Size#Small");
        refine(L"Medium (1–128 MB)", L"System.Size:System.Size#Medium");
        refine(L"Large (128 MB–1 GB)", L"System.Size:System.Size#Large");
        refine(L"Huge (1–4 GB)", L"System.Size:System.Size#Huge");
        refine(L"Gigantic (over 4 GB)", L"System.Size:System.Size#Gigantic");
    }
    if (!anchor) anchor = command == FileMenu ? window_ : command == HistoryMenu ? nav_ : window_;
    RECT rect{};
    if (anchor) GetWindowRect(anchor, &rect);
    else { POINT point{}; GetCursorPos(&point); rect.left = point.x; rect.bottom = point.y; }
    if (command == QuickAccessMenu && false) {
        RECT button{};
        if (SendMessageW(window_, TB_GETRECT, QuickAccessMenu, reinterpret_cast<LPARAM>(&button))) {
            MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&button), 2);
            rect.left = button.left; rect.bottom = button.bottom;
        }
    }
    const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    const bool libraryEdit = (selected >= 35000 && selected - 35000 < libraryFolders.size()) ||
        (selected >= 36000 && selected - 36000 < libraryFolders.size()) ||
        (command == LibraryOptimize && selected >= 37000 && selected - 37000 < libraryKinds().size());
    if (libraryEdit && (navigating_ || libraryGeneration != navigationCount_ || !libraryTarget || !currentPidl_ ||
        !ILIsEqual(libraryTarget.get(), currentPidl_.get()))) return;
    bool toolbarChanged = false;
    const auto catalog = quickAccessCommands();
    const auto& commands = quickAccessModel_.commands();
    if (selected >= 34000 && selected - 34000 < libraryFolders.size()) {
        const auto& location = libraryFolders[selected - 34000];
        showError(location.path.empty() ? browser_->BrowseToObject(location.item.Get(), SBSP_ABSOLUTE) : navigate(location.path.wstring()), L"Open library location");
    } else if (selected >= 35000 && selected - 35000 < libraryFolders.size()) {
        const auto hr = library_.setDefaultSaveFolder(libraryFolders[selected - 35000].item.Get());
        showError(FAILED(hr) ? hr : commitLibrary(), L"Set library save location");
    } else if (selected >= 36000 && selected - 36000 < libraryFolders.size()) {
        const auto hr = library_.removeFolder(libraryFolders[selected - 36000].item.Get());
        showError(FAILED(hr) ? hr : commitLibrary(), L"Remove library location");
    } else if (command == LibraryOptimize && selected >= 37000 && selected - 37000 < libraryKinds().size()) {
        const auto hr = library_.optimize(libraryKinds()[selected - 37000].kind);
        showError(FAILED(hr) ? hr : commitLibrary(), L"Optimize library");
    } else if (command == QuickAccessMenu && selected >= 30000 && selected - 30000 < catalog.size()) {
        const auto id = catalog[selected - 30000].command;
        toolbarChanged = quickAccessModel_.contains(id) ? quickAccessModel_.remove(id) : quickAccessModel_.add(id);
    } else if (command == QuickAccessMenu && selected >= 31000 && selected - 31000 < commands.size()) {
        const auto index = selected - 31000;
        toolbarChanged = index && quickAccessModel_.move(commands[index], index - 1);
    } else if (command == QuickAccessMenu && selected >= 32000 && selected - 32000 < commands.size()) {
        const auto index = selected - 32000;
        toolbarChanged = index + 1 < commands.size() && quickAccessModel_.move(commands[index], index + 1);
    } else if (command == QuickAccessMenu && selected >= 33000 && selected - 33000 < commands.size()) {
        toolbarChanged = quickAccessModel_.remove(commands[selected - 33000]);
    } else if (selected >= 4000 && selected - 4000 < history_.size()) {
        pendingHistory_ = selected - 4000;
        auto hr = browser_->BrowseToIDList(history_[pendingHistory_].get(), SBSP_ABSOLUTE);
        if (FAILED(hr)) pendingHistory_ = -1;
        showError(hr, L"Open history location");
    } else if (selected >= 26000 && selected - 26000 < columnKeys.size()) {
        showError(toggleColumn(columnKeys[selected - 26000]), L"Change columns");
    } else if (selected >= 27000 && selected - 27000 < recentSearches_.size()) {
        setSearchText(recentSearches_[selected - 27000]);
        showError(execute(Search), L"Search");
    } else if (selected >= 28000 && selected - 28000 < refinements.size()) {
        const auto previous = trim(textOf(search_));
        const size_t category = command == SearchKindMenu ? 0 : command == SearchDateMenu ? 1 : 2;
        showError(startSearch(previous, searchRecursive_, category, refinements[selected - 28000]), L"Refine search");
    } else if (selected) showError(execute(selected), L"Command");
    if (toolbarChanged) rebuildQuickAccess();
}
void ExplorerApp::showError(HRESULT hr, const wchar_t* action) {
    if (SUCCEEDED(hr) || isShellOperationCancelled(hr)) return;
    lastError_ = std::wstring(action) + L": " + hresultMessage(hr);
    setStatus(lastError_);
    if (!headless_) MessageBoxW(window_, lastError_.c_str(), L"Windows Explorer", MB_OK | MB_ICONERROR);
}
void ExplorerApp::persist() {
    if (headless_) return;
    if(searchSuggestionsAllowed_)saveSearchHistory(searchHistoryPath(),recentSearches_);
    if(typedAddressHistoryAllowed_)addressHistoryStatus_=saveAddressHistory(addressHistoryPath(),typedAddresses_);
    RECT rect{}; GetWindowRect(window_, &rect);
    if (fullscreen_) {
        preferences_.windowWidth = windowRect_.right - windowRect_.left;
        preferences_.windowHeight = windowRect_.bottom - windowRect_.top;
    } else if (!IsIconic(window_)) {
        preferences_.windowWidth = rect.right - rect.left;
        preferences_.windowHeight = rect.bottom - rect.top;
    }
    bool minimized = false;
    if (SUCCEEDED(ribbon_.minimized(minimized))) preferences_.ribbonCollapsed = minimized;
    preferences_.expandToCurrent=expandCurrent_; preferences_.showAllFolders=showAllFolders_; preferences_.showLibraries=showLibraries_;
    savePreferences(preferencesPath(), preferences_);
    const auto settings = preferencesPath();
    if (!settings.empty()) ribbon_.saveSettings(settings.parent_path() / L"ribbon.bin");
}
bool ExplorerApp::preprocess(MSG& message) {
    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const auto key = message.wParam;
        auto focus = GetFocus();
        wchar_t focusClass[128]{};
        if (focus) GetClassNameW(focus, focusClass, 128);
        const bool inEdit = focus == address_ || focus == search_ || _wcsicmp(focusClass, L"Edit") == 0 ||
                            _wcsnicmp(focusClass, L"RichEdit", 8) == 0;
        if (alt && key == VK_F4) return false;
        const auto command = shortcutCommand(static_cast<UINT>(key), control, shift, alt, inEdit);
        if (command) { showError(execute(*command), L"Command"); return true; }
    }
    HWND nativeView = nullptr;
    if (view_ && SUCCEEDED(view_->GetWindow(&nativeView)) &&
        (message.hwnd == nativeView || IsChild(nativeView, message.hwnd)) &&
        view_->TranslateAccelerator(&message) == S_OK) return true;
    if (message.message == WM_KEYDOWN && message.wParam == VK_TAB &&
        !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        auto next = GetNextDlgTabItem(window_, GetFocus(), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        if (next) { SetFocus(next); return true; }
    }
    return false;
}
int ExplorerApp::run(int showCommand) {
    ShowWindow(window_, showCommand); UpdateWindow(window_);
    if (view_) view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (preprocess(message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
LRESULT CALLBACK ExplorerApp::windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto app = reinterpret_cast<ExplorerApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<ExplorerApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) {
        try { return app->onMessage(message, wparam, lparam); }
        catch (...) { app->showError(E_FAIL, L"Window command"); return 0; }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK ExplorerApp::editProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto app = reinterpret_cast<ExplorerApp*>(data);
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        if (id == Address) app->finishAddress(true);
        else app->showError(app->execute(Search), L"Search");
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        if (id == Address) app->finishAddress(false);
        else {
            app->liveSearchPolicy_.escape(GetTickCount64());app->setSearchText(L"",false);
            app->processLiveSearch();if(app->view_)app->view_->UIActivate(SVUIA_ACTIVATE_FOCUS);
        }
        return 0;
    }
    if (message == WM_KILLFOCUS && id == Address && app->addressEditing_) {
        app->addressEditing_ = false;
        ShowWindow(app->address_, SW_HIDE); ShowWindow(app->breadcrumbs_, SW_SHOW);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
LRESULT ExplorerApp::onMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    { LRESULT result = 0;
      if (activeNamespaceMenu_ && activeNamespaceMenu_->handleMenuMessage(message, wparam, lparam, result)) return result;
      if (namespaceActions_.handleMenuMessage(message, wparam, lparam, result)) return result; }
    {LRESULT result=0;if(backgroundActions_.handleMenuMessage(message,wparam,lparam,result))return result;}
    if (activeContextMenu_) {
        LRESULT result = 0;
        if (activeContextMenu_->handleMessage(message, wparam, lparam, result)) return result;
    }
    switch (message) {
    case WM_SIZE: layout(); return 0;
    case WM_CLIPBOARDUPDATE:
        cancelCommandStates();namespaceDirty_=true;
        if(!closing_&&!navigating_)updateCommands();
        return 0;
    case WM_SETCURSOR:
        if(LOWORD(lparam)==HTCLIENT) {
            POINT cursor{};GetCursorPos(&cursor);ScreenToClient(window_,&cursor);
            RECT searchBounds{};GetWindowRect(search_,&searchBounds);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&searchBounds),2);
            if(searchResizing_ || (cursor.x>=searchBounds.left-px(9)&&cursor.x<searchBounds.left&&cursor.y>=searchBounds.top&&cursor.y<searchBounds.bottom)) {
                SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT cursor{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        RECT searchBounds{};GetWindowRect(search_,&searchBounds);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&searchBounds),2);
        if(cursor.x>=searchBounds.left-px(9)&&cursor.x<searchBounds.left&&cursor.y>=searchBounds.top&&cursor.y<searchBounds.bottom) {
            searchResizing_=true;searchDragOffset_=searchBounds.left-cursor.x;SetCapture(window_);return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if(searchResizing_&&GetCapture()==window_) {
            RECT bounds{};GetClientRect(window_,&bounds);
            const auto physical=bounds.right-GET_X_LPARAM(lparam)-searchDragOffset_-px(12);
            preferences_.searchWidth=std::clamp(MulDiv(physical,96,static_cast<int>(dpi_)),90,4096);
            layout();return 0;
        }
        break;
    case WM_LBUTTONUP:
        if(searchResizing_) {searchResizing_=false;if(GetCapture()==window_)ReleaseCapture();persist();return 0;}
        break;
    case WM_CAPTURECHANGED: searchResizing_=false;break;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {px(600), px(320)}; return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        updateCaptionIcon();
        auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);
        auto old = font_; font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        for (auto hwnd : {nav_, address_, breadcrumbs_, search_, status_, statusView_, addressActions_}) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        applyChrome(nav_, breadcrumbs_, address_, search_, statusView_, addressActions_);
        applyWindowTheme(window_); applyRibbonTheme(ribbon_.framework());
        ribbon_.invalidate(); rebuildRibbon(); updateBreadcrumbs(); layout(); if (old) DeleteObject(old);
        return 0;
    }
    case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
        refreshProcessTheme(); applyWindowTheme(window_); applyRibbonTheme(ribbon_.framework());
        applyChrome(nav_, breadcrumbs_, address_, search_, statusView_, addressActions_);
        if(!headless_) {
            refreshAddressHistoryPolicy();
            refreshCabinetPolicy();updateFrameTitle();
            if(browser_) {
                EXPLORER_BROWSER_OPTIONS options{};
                if(SUCCEEDED(browser_->GetOptions(&options))) {
                    options=static_cast<EXPLORER_BROWSER_OPTIONS>(saveLocalView_?options&~EBO_NOPERSISTVIEWSTATE:options|EBO_NOPERSISTVIEWSTATE);
                    browser_->SetOptions(options);
                }
            }
            SHELLSTATE settings{};
            SHGetSetSettings(&settings,SSF_SHOWALLOBJECTS|SSF_SHOWSUPERHIDDEN|SSF_SHOWEXTENSIONS,FALSE);
            const bool changed=preferences_.showHidden!=static_cast<bool>(settings.fShowAllObjects) || showSuperHidden_!=static_cast<bool>(settings.fShowSuperHidden);
            preferences_.showHidden=settings.fShowAllObjects;preferences_.showExtensions=settings.fShowExtensions;showSuperHidden_=settings.fShowSuperHidden;
            if(changed&&view_)view_->Refresh();
            const bool allowed=searchSuggestionsAllowed();
            if(allowed!=searchSuggestionsAllowed_) {
                searchSuggestionsAllowed_=allowed;
                if(!allowed) {
                    if(searchAutocomplete_)searchAutocomplete_->Enable(FALSE);
                    recentSearches_.clear();if(searchSuggestions_)searchSuggestions_->replace(recentSearches_);
                } else {
                    loadSearchHistory(searchHistoryPath(),&recentSearches_);
                    if(!searchSuggestions_)searchSuggestions_.Attach(new SearchSuggestionList());
                    searchSuggestions_->replace(recentSearches_);
                    if(searchAutocomplete_)searchAutocomplete_->Enable(TRUE);
                    else attachSearchSuggestions(search_,searchSuggestions_.Get(),false,&searchAutocomplete_);
                }
            }
        }
        namespaceDirty_ = selectionStateDirty_ = true;
        ribbon_.invalidate(); InvalidateRect(window_, nullptr, TRUE); break;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        if (const auto brush = themeControlColor(reinterpret_cast<HWND>(lparam), reinterpret_cast<HDC>(wparam), message))
            return reinterpret_cast<LRESULT>(brush);
        break;
    case WM_COMMAND:
        if(reinterpret_cast<HWND>(lparam)==search_&&HIWORD(wparam)==EN_CHANGE) {
            if(!suppressSearchChanges_&&!closing_) {
                ++searchInteractionRevision_;
                liveSearchStatus_=liveSearchPolicy_.userEdited(textOf(search_),GetTickCount64());
                if(SUCCEEDED(liveSearchStatus_)) {
                    scheduleLiveSearch();
                    const auto deadline=liveSearchPolicy_.deadline();
                    if(deadline&&*deadline<=GetTickCount64())processLiveSearch();
                }
            }
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED || lparam == 0) {
            auto command = LOWORD(wparam);
            if (command != Search && command != Address) showError(execute(command), L"Command");
            else if (command == Address && lparam == 0) editAddress();
        }
        return 0;
    case WM_NOTIFY: {
        auto notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW && (notification->hwndFrom == nav_ || notification->hwndFrom == statusView_ || notification->hwndFrom == addressActions_))
            return chromeToolbarCustomDraw(*reinterpret_cast<NMTBCUSTOMDRAW*>(lparam), notification->hwndFrom == statusView_, dpi_);
        if(notification->code==NM_CUSTOMDRAW&&notification->hwndFrom==breadcrumbs_)
            return chromeBreadcrumbCustomDraw(*reinterpret_cast<NMTBCUSTOMDRAW*>(lparam),dpi_);
        if (notification->hwndFrom==breadcrumbs_ && notification->code==TBN_DROPDOWN) {
            const auto dropdown=reinterpret_cast<NMTOOLBARW*>(lparam);
            beginBreadcrumbMenu(static_cast<UINT>(dropdown->iItem)); return TBDDRET_NODEFAULT;
        }
        return 0;
    }
    case WM_TIMER:
        if(wparam==6) {if(!closing_)processLiveSearch();return 0;}
        if (wparam==2) { pollBreadcrumbMenu(); return 0; }
        if (wparam==3) { advanceNavigationExpansion(); return 0; }
        if (wparam==4) { pollCommandStates(); return 0; }
        if (wparam==5) { pollFrequentPlaces(); return 0; }
        if (!closing_) {
            if (archiveTask_.valid() && archiveTask_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const auto hr = archiveTask_.get();
                if (FAILED(hr)) showError(hr, archiveAction_.c_str());
                else if (view_) view_->Refresh();
            }
            applyPendingSelection(); updateStatus(); updateCommands();
        }
        return 0;
    case WM_APPCOMMAND:
        if (GET_APPCOMMAND_LPARAM(lparam) == APPCOMMAND_BROWSER_BACKWARD) { execute(Back); return TRUE; }
        if (GET_APPCOMMAND_LPARAM(lparam) == APPCOMMAND_BROWSER_FORWARD) { execute(Forward); return TRUE; }
        break;
    case DeferredUpdate:
        deferredUpdateQueued_=false;
        if(!closing_) {applyPendingSelection();updateStatus();updateCommands();}
        return 0;
    case WM_DRAWITEM: {
        const auto draw=reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if(draw&&draw->hwndItem==ribbonCollapse_) {
            bool minimized=false;
            if(FAILED(ribbon_.minimized(minimized)))minimized=preferences_.ribbonCollapsed;
            return drawRibbonCollapseButton(*draw,minimized,dpi_)?TRUE:FALSE;
        }
        break;
    }
    case ShareResult:
        if (!closing_ && static_cast<HRESULT>(wparam) != E_PENDING && FAILED(static_cast<HRESULT>(wparam)))
            showError(static_cast<HRESULT>(wparam), L"Prepare files for sharing");
        return 0;
    case NamespaceResult:
        if(closing_)return 0;
        if (FAILED(static_cast<HRESULT>(lparam))) showError(static_cast<HRESULT>(lparam), L"Offline files");
        namespaceDirty_ = true; updateCommands(); return 0;
    case DeferredView:
        if (!closing_&&folderView_ && !navigating_) {
            if(headless_)folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, checkboxes_ ? FWF_CHECKSELECT : 0);
            else { DWORD flags{};if(SUCCEEDED(folderView_->GetCurrentFolderFlags(&flags)))checkboxes_=(flags&FWF_CHECKSELECT)!=0; }
            // Quick access is an aggregate home page; Windows 10 presents its
            // category headers without a standalone Name/Type column header.
            if(quickAccessLocation_)
                folderView_->SetCurrentFolderFlags(FWF_NOCOLUMNHEADER,FWF_NOCOLUMNHEADER);
            if(searchPresentationPending_) {
                searchPresentationPending_=false;
                if(searchPresentation_) {
                    const auto applied=applySearchViewPresentation(folderView_.Get(),*searchPresentation_);
                    if(FAILED(applied))searchPresentationStatus_=applied;
                }
                showError(searchPresentationStatus_,L"Restore saved search view");
            }
            applyPendingSelection(); applyWindowTheme(window_);layout();
            applyNavigationOptions(); initializeBreadcrumbDrop();
        }
        return 0;
    case WM_CLOSE:
        cancelLiveSearch();
        closing_=true;cancelCommandStates();cancelFrequentPlaces();
        if(searchAutocomplete_)searchAutocomplete_->Enable(FALSE);
        searchAutocomplete_.Reset();searchSuggestions_.Reset();
        persist();DestroyWindow(window_);return 0;
    case WM_DESTROY:
        cancelLiveSearch();
        RemoveClipboardFormatListener(window_);cancelCommandStates();
        closing_=true;KillTimer(window_,1);cancelFrequentPlaces();
        if(breadcrumbTask_) {breadcrumbTask_->cancel();breadcrumbTask_.reset();}
        shutdownStatus_=drainStaWorkers(5000);
        if(SUCCEEDED(shutdownStatus_)) {
            // The native frequent-place pin callback can arrive on Destroy.
            // Keep its original Shell view site alive until that callback ends.
            nativeShare_.reset();forgetRibbonTheme(ribbon_.framework());ribbon_.reset();destroyBrowser();
        }
        if (!headless_) PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        const auto old=window_;SetWindowLongPtrW(old,GWLP_USERDATA,0);window_=nullptr;
        return DefWindowProcW(old,message,wparam,lparam);
    }
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}


}
