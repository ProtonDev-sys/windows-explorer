#include "explorer/app.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/search.hpp"
#include "explorer/extra_operations.hpp"
#include "explorer/input.hpp"
#include <windowsx.h>
#include <uxtheme.h>
#include <propkey.h>
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
constexpr int TabsId = 900;
bool visibleWindowObserved = false;
constexpr std::array<const wchar_t*, 8> ViewNames{
    L"Extra large icons", L"Large icons", L"Medium icons", L"Small icons",
    L"List", L"Details", L"Tiles", L"Content"};

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
bool pumpUntil(const std::function<bool()>& ready, DWORD timeoutMs) {
    auto observeWindows = [] {
        EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
            DWORD process = 0; GetWindowThreadProcessId(hwnd, &process);
            if (process == GetCurrentProcessId() && IsWindowVisible(hwnd)) visibleWindowObserved = true;
            return TRUE;
        }, 0);
    };
    const auto end = GetTickCount64() + timeoutMs;
    do {
        observeWindows();
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            observeWindows();
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
    return ready();
}
std::string jsonString(const std::wstring& value) {
    auto bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), utf8.data(), bytes, nullptr, nullptr);
    std::string result = "\"";
    for (unsigned char c : utf8) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 0x20) { char escaped[7]; sprintf_s(escaped, "\\u%04x", c); result += escaped; }
        else result += c;
    }
    return result + '"';
}
HRESULT shellExecute(HWND owner, const wchar_t* target, const wchar_t* arguments = nullptr, const wchar_t* directory = nullptr) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner; info.lpVerb = L"open"; info.lpFile = target;
    info.lpParameters = arguments; info.lpDirectory = directory; info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
}

ExplorerApp::ExplorerApp(HINSTANCE instance, bool headless)
    : instance_(instance), headless_(headless), preferences_(headless ? Preferences{} : loadPreferences(preferencesPath())) {}

ExplorerApp::~ExplorerApp() {
    destroyBrowser();
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    for (auto images : ribbonImages_) ImageList_Destroy(images);
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
    if (searchActive_) *flags |= CDB2GVF_NOINCLUDEITEM;
    if (preferences_.showHidden) *flags |= CDB2GVF_SHOWALLFILES;
    return S_OK;
}
HRESULT ExplorerApp::ShouldShow(IShellFolder* folder, PCIDLIST_ABSOLUTE, PCUITEMID_CHILD item) {
    if (!folder || !item) return S_OK;
    SFGAOF attributes = SFGAO_HIDDEN | SFGAO_SYSTEM;
    if (FAILED(folder->GetAttributesOf(1, &item, &attributes))) return S_OK;
    if ((attributes & (SFGAO_HIDDEN | SFGAO_SYSTEM)) == (SFGAO_HIDDEN | SFGAO_SYSTEM)) return S_FALSE;
    return !preferences_.showHidden && (attributes & SFGAO_HIDDEN) ? S_FALSE : S_OK;
}
HRESULT ExplorerApp::GetEnumFlags(IShellFolder*, PCIDLIST_ABSOLUTE, HWND* owner, DWORD* flags) {
    if (!flags) return E_POINTER;
    if (owner) *owner = window_;
    if (preferences_.showHidden) *flags |= SHCONTF_INCLUDEHIDDEN;
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
    dpi_ = GetDpiForWindow(window_);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    createControls();
    auto hr = createBrowser();
    if (FAILED(hr)) { DestroyWindow(window_); return hr; }
    layout();
    hr = navigate(location.empty() ? preferences_.startupLocation : location);
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
    browser_->SetOptions(static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_SHOWFRAMES | EBO_NOTRAVELLOG));
    browser_->SetPropertyBag(L"WindowsExplorer.Native");
    FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
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
    folderView_.Reset(); view_.Reset();
    if (browser_) {
        ComPtr<IFolderFilterSite> filterSite;
        if (SUCCEEDED(browser_.As(&filterSite))) filterSite->SetFilter(nullptr);
        if (adviseCookie_) { browser_->Unadvise(adviseCookie_); adviseCookie_ = 0; }
        if (browserInitialized_) { browser_->Destroy(); browserInitialized_ = false; }
        ComPtr<IObjectWithSite> site;
        if (SUCCEEDED(browser_.As(&site))) site->SetSite(nullptr);
        browser_.Reset();
    }
}
HRESULT ExplorerApp::recreateBrowser() {
    Pidl location(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
    destroyBrowser();
    const auto hr = createBrowser();
    layout();
    if (FAILED(hr)) return hr;
    return location ? browser_->BrowseToIDList(location.get(), SBSP_ABSOLUTE) : navigate(preferences_.startupLocation);
}
HRESULT ExplorerApp::navigate(const std::wstring& location) {
    if (!browser_) return E_UNEXPECTED;
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
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromIDList(pidl.get(), IID_PPV_ARGS(&item)))) {
        SFGAOF attributes = 0;
        item->GetAttributes(SFGAO_FOLDER, &attributes);
        if (!(attributes & SFGAO_FOLDER)) return headless_ ? HRESULT_FROM_WIN32(ERROR_DIRECTORY) : shellExecute(window_, target.c_str());
    }
    pendingHistory_ = -1;
    return browser_->BrowseToIDList(pidl.get(), SBSP_ABSOLUTE);
}
HRESULT ExplorerApp::OnNavigationPending(PCIDLIST_ABSOLUTE pidl) {
    if (pendingHistory_ >= 0 && !ILIsEqual(history_[pendingHistory_].get(), pidl)) pendingHistory_ = -1;
    searchActive_ = std::any_of(searchLocations_.begin(), searchLocations_.end(),
                                [&](const Pidl& item) { return ILIsEqual(item.get(), pidl); });
    navigating_ = true; navigationStarted_ = GetTickCount64();
    setStatus(L"Loading…"); return S_OK;
}
HRESULT ExplorerApp::OnViewCreated(IShellView* view) {
    view_ = view;
    folderView_.Reset();
    if (view) view->QueryInterface(IID_PPV_ARGS(&folderView_));
    PostMessageW(window_, DeferredView, 0, 0);
    return S_OK;
}
HRESULT ExplorerApp::OnNavigationComplete(PCIDLIST_ABSOLUTE pidl) {
    navigating_ = false;
    lastNavigationMs_ = GetTickCount64() - navigationStarted_;
    ++navigationCount_;
    currentPidl_.reset(ILCloneFull(pidl));
    currentLocation_ = pidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING);
    currentName_ = pidlName(pidl, SIGDN_NORMALDISPLAY);
    if (pendingHistory_ >= 0 && ILIsEqual(history_[pendingHistory_].get(), pidl)) { historyIndex_ = pendingHistory_; pendingHistory_ = -1; }
    else if (historyIndex_ < 0 || !ILIsEqual(history_[historyIndex_].get(), pidl)) {
        pendingHistory_ = -1;
        history_.resize(static_cast<size_t>(historyIndex_ + 1));
        history_.emplace_back(ILCloneFull(pidl));
        historyIndex_ = static_cast<int>(history_.size()) - 1;
        if (history_.size() > 100) { history_.erase(history_.begin()); --historyIndex_; }
    }
    SetWindowTextW(window_, (currentName_ + L" — Windows Explorer").c_str());
    SetWindowTextW(address_, currentLocation_.c_str());
    SendMessageW(search_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>((L"Search " + currentName_).c_str()));
    updateBreadcrumbs();
    PostMessageW(window_, DeferredView, 0, 0);
    PostMessageW(window_, DeferredUpdate, 0, 0);
    return S_OK;
}
HRESULT ExplorerApp::OnNavigationFailed(PCIDLIST_ABSOLUTE) {
    navigating_ = false; pendingHistory_ = -1;
    lastError_ = L"This location could not be opened. Check its availability and your permissions.";
    setStatus(lastError_);
    return S_OK;
}
HRESULT ExplorerApp::OnStateChange(IShellView*, ULONG) {
    PostMessageW(window_, DeferredUpdate, 0, 0); return S_OK;
}

void ExplorerApp::createControls() {
    auto control = [&](const wchar_t* type, const wchar_t* label, DWORD style, UINT id) {
        auto hwnd = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                   window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return hwnd;
    };
    file_ = control(L"BUTTON", L"File", BS_PUSHBUTTON | WS_TABSTOP, FileMenu);
    tabs_ = control(WC_TABCONTROLW, L"", TCS_FOCUSNEVER | TCS_FIXEDWIDTH, TabsId);
    for (auto label : {L"Home", L"Share", L"View", L"Computer"}) {
        TCITEMW tab{TCIF_TEXT}; tab.pszText = const_cast<LPWSTR>(label);
        TabCtrl_InsertItem(tabs_, TabCtrl_GetItemCount(tabs_), &tab);
    }
    SendMessageW(tabs_, TCM_SETITEMSIZE, 0, MAKELPARAM(px(78), px(25)));
    nav_ = control(TOOLBARCLASSNAMEW, L"Navigation", TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 901);
    SendMessageW(nav_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    auto navButton = [&](UINT id, const wchar_t* label, BYTE style = BTNS_BUTTON) {
        TBBUTTON button{}; button.iBitmap = I_IMAGENONE; button.idCommand = id;
        button.fsState = TBSTATE_ENABLED; button.fsStyle = style | BTNS_AUTOSIZE;
        button.iString = reinterpret_cast<INT_PTR>(label);
        SendMessageW(nav_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button));
    };
    navButton(Back, L"←"); navButton(Forward, L"→"); navButton(HistoryMenu, L"▾");
    navButton(Up, L"↑"); navButton(Refresh, L"↻");
    breadcrumbs_ = control(TOOLBARCLASSNAMEW, L"Location breadcrumbs", TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 902);
    SendMessageW(breadcrumbs_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    address_ = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, Address);
    ShowWindow(address_, SW_HIDE);
    SetWindowSubclass(address_, editProc, Address, reinterpret_cast<DWORD_PTR>(this));
    search_ = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, Search);
    SetWindowSubclass(search_, editProc, Search, reinterpret_cast<DWORD_PTR>(this));
    status_ = control(STATUSCLASSNAMEW, L"Ready", SBARS_SIZEGRIP, 903);
    SetTimer(window_, 1, 700, nullptr);
    rebuildRibbon();
}

void ExplorerApp::rebuildRibbon() {
    for (auto control : ribbonControls_) DestroyWindow(control);
    ribbonControls_.clear();
    for (auto images : ribbonImages_) ImageList_Destroy(images);
    ribbonImages_.clear();
    if (preferences_.ribbonCollapsed) { layout(); return; }
    int x = px(8), top = px(30), height = px(94);
    auto child = [&](const wchar_t* type, const wchar_t* label, DWORD style, UINT id, int left, int y, int width, int h) {
        auto handle = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, left, y, width, h,
                                     window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        ribbonControls_.push_back(handle);
        return handle;
    };
    auto group = [&](const wchar_t* name, int width, const std::function<void(int)>& contents) {
        contents(x);
        child(L"STATIC", name, SS_CENTER, 0, x, top + px(73), px(width - 8), px(18));
        child(L"STATIC", L"", SS_ETCHEDVERT, 0, x + px(width - 4), top + px(3), px(1), height - px(7));
        x += px(width);
    };
    auto smallButton = [&](UINT id, const wchar_t* label, int left, int row, int width = 100, bool checked = false, bool toggle = false) {
        auto hwnd = child(L"BUTTON", label, WS_TABSTOP | (toggle ? BS_AUTOCHECKBOX : BS_PUSHBUTTON), id,
                          left, top + px(row * 24), px(width), px(23));
        if (toggle) SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    auto large = [&](UINT id, const wchar_t* label, int left, SHSTOCKICONID icon) {
        auto hwnd = child(L"BUTTON", label, WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, id, left, top, px(64), px(70));
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(SHGetStockIconInfo(icon, SHGSI_ICON | SHGSI_LARGEICON, &info))) {
            auto list = ImageList_Create(px(32), px(32), ILC_COLOR32 | ILC_MASK, 1, 0);
            ImageList_AddIcon(list, info.hIcon); DestroyIcon(info.hIcon);
            BUTTON_IMAGELIST images{}; images.himl = list; images.uAlign = BUTTON_IMAGELIST_ALIGN_TOP;
            images.margin = {0, px(5), 0, px(2)};
            SendMessageW(hwnd, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&images));
            ribbonImages_.push_back(list);
        }
    };
    const auto selectedTab = TabCtrl_GetCurSel(tabs_);
    if (selectedTab == 0) {
        group(L"Clipboard", 240, [&](int left) {
            large(Pin, L"Pin to\nQuick access", left, SIID_FOLDER);
            large(Copy, L"Copy", left + px(65), SIID_DOCNOASSOC);
            large(Paste, L"Paste", left + px(130), SIID_DOCASSOC);
            smallButton(Cut, L"Cut", left + px(195), 0, 40);
            smallButton(CopyPath, L"Path", left + px(195), 1, 40);
            smallButton(PasteShortcut, L"Link", left + px(195), 2, 40);
        });
        group(L"Organize", 218, [&](int left) {
            smallButton(MoveTo, L"Move to…", left, 0, 82); smallButton(CopyTo, L"Copy to…", left, 1, 82);
            large(Delete, L"Delete", left + px(85), SIID_DELETE);
            large(Rename, L"Rename", left + px(150), SIID_RENAME);
        });
        group(L"New", 152, [&](int left) {
            large(NewFolder, L"New folder", left, SIID_FOLDER);
            smallButton(NewText, L"Text file", left + px(68), 0, 76);
            smallButton(NewShortcut, L"Shortcut…", left + px(68), 1, 76);
            smallButton(FolderOptions, L"Options", left + px(68), 2, 76);
        });
        group(L"Open", 152, [&](int left) {
            large(Properties, L"Properties", left, SIID_INFO);
            smallButton(Open, L"Open", left + px(68), 0, 76);
            smallButton(Edit, L"Edit", left + px(68), 1, 76);
            smallButton(FileHistory, L"History", left + px(68), 2, 76);
        });
        group(L"Select", 104, [&](int left) {
            smallButton(SelectAll, L"Select all", left, 0, 96); smallButton(SelectNone, L"Select none", left, 1, 96);
            smallButton(Invert, L"Invert selection", left, 2, 96);
        });
    } else if (selectedTab == 1) {
        group(L"Send", 280, [&](int left) {
            large(Sharing, L"Sharing", left, SIID_SHARE); large(Zip, L"Zip", left + px(68), SIID_ZIPFILE);
            large(Print, L"Print", left + px(136), SIID_PRINTER); large(Extract, L"Extract all", left + px(204), SIID_FOLDER);
        });
        group(L"Give access to", 140, [&](int left) { large(Security, L"Advanced\nsecurity", left, SIID_LOCK); });
    } else if (selectedTab == 2) {
        group(L"Panes", 165, [&](int left) {
            smallButton(NavigationPane, L"Navigation pane", left, 0, 155, preferences_.navigationPane, true);
            smallButton(PreviewPane, L"Preview pane", left, 1, 155, preferences_.previewPane, true);
            smallButton(DetailsPane, L"Details pane", left, 2, 155, preferences_.detailsPane, true);
        });
        group(L"Layout", 283, [&](int left) {
            for (int i = 0; i < 8; ++i) {
                smallButton(ViewFirst + i, ViewNames[i], left + px((i / 3) * 91), i % 3, 89);
            }
        });
        group(L"Current view", 122, [&](int left) {
            smallButton(SortMenu, L"Sort by ▾", left, 0, 112); smallButton(GroupMenu, L"Group by ▾", left, 1, 112);
            smallButton(Refresh, L"Refresh", left, 2, 112);
        });
        group(L"Show/hide", 183, [&](int left) {
            smallButton(Checkboxes, L"Item check boxes", left, 0, 173, checkboxes_, true);
            smallButton(Extensions, L"File name extensions", left, 1, 173, preferences_.showExtensions, true);
            smallButton(HiddenItems, L"Hidden items", left, 2, 173, preferences_.showHidden, true);
        });
        group(L"Options", 100, [&](int left) { large(FolderOptions, L"Options", left, SIID_SETTINGS); });
    } else {
        group(L"Locations", 280, [&](int left) {
            large(ThisPC, L"This PC", left, SIID_DESKTOPPC); large(QuickAccess, L"Quick access", left + px(68), SIID_FOLDER);
            large(Network, L"Network", left + px(136), SIID_MYNETWORK); large(RecycleBin, L"Recycle Bin", left + px(204), SIID_RECYCLER);
        });
        group(L"Network", 180, [&](int left) {
            smallButton(MapDrive, L"Map network drive…", left, 0, 170);
            smallButton(DisconnectDrive, L"Disconnect drive…", left, 1, 170);
        });
        group(L"System", 180, [&](int left) {
            smallButton(Terminal, L"Open command prompt", left, 0, 170);
            smallButton(FolderOptions, L"Folder options", left, 1, 170);
            smallButton(FileHistory, L"File History", left, 2, 170);
        });
    }
    updateCommands();
    layout();
}

void ExplorerApp::layout() {
    if (!window_ || !tabs_) return;
    RECT client{}; GetClientRect(window_, &client);
    const int width = client.right, height = client.bottom;
    MoveWindow(file_, px(4), px(1), px(57), px(26), TRUE);
    MoveWindow(tabs_, px(66), 0, std::max(px(350), width - px(70)), px(28), TRUE);
    const int y = preferences_.ribbonCollapsed ? px(34) : px(130);
    const int searchWidth = std::clamp(width / 4, px(160), px(300));
    const int navWidth = px(145);
    MoveWindow(nav_, px(4), y, navWidth - px(4), px(30), TRUE);
    const int addressWidth = std::max(px(100), width - navWidth - searchWidth - px(24));
    MoveWindow(address_, navWidth, y + px(1), addressWidth, px(27), TRUE);
    MoveWindow(breadcrumbs_, navWidth, y, addressWidth, px(30), TRUE);
    MoveWindow(search_, navWidth + addressWidth + px(10), y + px(1), searchWidth, px(27), TRUE);
    SendMessageW(status_, WM_SIZE, 0, 0);
    int parts[]{std::max(px(100), width - px(185)), -1};
    SendMessageW(status_, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    RECT statusRect{}; GetWindowRect(status_, &statusRect);
    if (browser_) {
        RECT viewRect{0, y + px(35), width, std::max(y + px(36), height - static_cast<int>(statusRect.bottom - statusRect.top))};
        browser_->SetRect(nullptr, viewRect);
    }
}

void ExplorerApp::updateBreadcrumbs() {
    while (SendMessageW(breadcrumbs_, TB_BUTTONCOUNT, 0, 0)) SendMessageW(breadcrumbs_, TB_DELETEBUTTON, 0, 0);
    breadcrumbsPidls_.clear();
    if (!currentPidl_) return;
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
        if (i + 1 < breadcrumbsPidls_.size()) label += L"  ›";
        TBBUTTON button{}; button.iBitmap = I_IMAGENONE;
        button.idCommand = BreadcrumbFirst + static_cast<int>(i);
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
        button.iString = reinterpret_cast<INT_PTR>(label.c_str());
        SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button));
    }
    TBBUTTON edit{}; edit.iBitmap = I_IMAGENONE; edit.idCommand = Address;
    edit.fsState = TBSTATE_ENABLED; edit.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    edit.iString = reinterpret_cast<INT_PTR>(L"   ▾");
    SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&edit));
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
    if (navigateNow) showError(navigate(value), L"Open location");
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
    if (!nav_) return;
    SendMessageW(nav_, TB_ENABLEBUTTON, Back, MAKELONG(historyIndex_ > 0, 0));
    SendMessageW(nav_, TB_ENABLEBUTTON, Forward, MAKELONG(historyIndex_ >= 0 && historyIndex_ + 1 < static_cast<int>(history_.size()), 0));
    SendMessageW(nav_, TB_ENABLEBUTTON, Up, MAKELONG(currentPidl_ && !ILIsEmpty(currentPidl_.get()), 0));
    ComPtr<IShellItemArray> selected;
    DWORD count = 0;
    if (SUCCEEDED(selection(selected)) && selected) selected->GetCount(&count);
    ComPtr<IShellItem> folder;
    bool writable = false;
    if (SUCCEEDED(currentFolder(folder))) {
        SFGAOF attrs = 0; folder->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attrs);
        writable = (attrs & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) == (SFGAO_FILESYSTEM | SFGAO_FOLDER);
    }
    SHELLSTATE shellSettings{};
    SHGetSetSettings(&shellSettings, SSF_SHOWEXTENSIONS, FALSE);
    preferences_.showExtensions = shellSettings.fShowExtensions;
    for (auto hwnd : ribbonControls_) {
        const auto id = GetDlgCtrlID(hwnd);
        bool enabled = true;
        if (id == Copy || id == Cut || id == CopyPath || id == MoveTo || id == CopyTo || id == Delete || id == Open || id == Edit || id == Print || id == Zip) enabled = count > 0;
        if (id == Rename) enabled = count == 1;
        if (id == NewFolder || id == NewText || id == NewShortcut || id == Paste || id == PasteShortcut || id == Terminal) enabled = writable;
        if (id == Extract) {
            ComPtr<IShellItem> item;
            if (count == 1 && SUCCEEDED(selected->GetItemAt(0, &item))) {
                auto name = itemName(item.Get(), SIGDN_FILESYSPATH);
                enabled = name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".zip") == 0;
            } else enabled = false;
        }
        if (id == Extensions) SendMessageW(hwnd, BM_SETCHECK, preferences_.showExtensions ? BST_CHECKED : BST_UNCHECKED, 0);
        if ((id == Zip || id == Extract) && archiveTask_.valid()) enabled = false;
        EnableWindow(hwnd, enabled);
    }
}
void ExplorerApp::setStatus(const std::wstring& text) {
    if (status_) SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
void ExplorerApp::updateStatus() {
    if (archiveTask_.valid()) { setStatus(archiveAction_ + L"…"); return; }
    if (navigating_ || !folderView_) return;
    int items = 0;
    folderView_->ItemCount(SVGIO_ALLVIEW, &items);
    ComPtr<IShellItemArray> selected;
    DWORD count = 0;
    if (SUCCEEDED(selection(selected)) && selected) selected->GetCount(&count);
    auto label = std::to_wstring(items) + (items == 1 ? L" item" : L" items");
    if (count) label += L"  |  " + std::to_wstring(count) + L" selected";
    setStatus(label);
    auto performance = std::to_wstring(lastNavigationMs_) + L" ms navigation";
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(performance.c_str()));
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
    if (SUCCEEDED(hr)) preferences_.view = mode;
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
    return SUCCEEDED(hr) ? ShellOperations::invoke(window_, items.Get(), verb) : hr;
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
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder);
    if (FAILED(hr)) return hr;
    const auto base = itemName(folder.Get(), SIGDN_FILESYSPATH);
    if (base.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::wstring name = L"New folder";
    std::error_code error;
    unsigned i = 2;
    while (std::filesystem::exists(std::filesystem::path(base) / name, error)) {
        if (i > 10000) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
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
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder);
    if (FAILED(hr)) return hr;
    auto base = itemName(folder.Get(), SIGDN_FILESYSPATH);
    if (base.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::wstring name = L"New Text Document.txt";
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned i = 1; i < 10000; ++i) {
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
    ComPtr<IShellItem> folder;
    auto hr = currentFolder(folder); if (FAILED(hr)) return hr;
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
    case FocusSearch: SetFocus(search_); SendMessageW(search_, EM_SETSEL, 0, -1); return S_OK;
    case Search: {
        auto query = trim(textOf(search_));
        if (query.empty()) return S_FALSE;
        currentFolder(folder);
        ComPtr<IShellItem> results;
        auto hr = createSearchFolder(query, folder.Get(), &results);
        if (FAILED(hr)) return hr;
        PIDLIST_ABSOLUTE raw = nullptr;
        hr = SHGetIDListFromObject(results.Get(), &raw);
        if (FAILED(hr)) return hr;
        searchLocations_.emplace_back(raw);
        if (searchLocations_.size() > 100) searchLocations_.erase(searchLocations_.begin());
        searchActive_ = true;
        return browser_->BrowseToObject(results.Get(), SBSP_ABSOLUTE);
    }
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
    case FileMenu: case HistoryMenu: case ViewMenu: case SortMenu: case GroupMenu: popup(command); return S_OK;
    case Close: PostMessageW(window_, WM_CLOSE, 0, 0); return S_OK;
    case NewWindow: {
        if (headless_) return E_ACCESSDENIED;
        std::wstring exe(32768, 0); exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
        // Shell namespace paths never contain quotes; reject a quoted user-controlled path before process creation.
        if (currentLocation_.find(L'"') != std::wstring::npos) return E_INVALIDARG;
        return shellExecute(window_, exe.c_str(), (L"--path " + quoteArgument(currentLocation_)).c_str());
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
    case CopyTo: return chooseDestination(false);
    case MoveTo: return chooseDestination(true);
    case Delete: case PermanentDelete: {
        if (headless_) return E_ACCESSDENIED;
        const auto hr = selection(items); if (FAILED(hr)) return hr;
        if (command == PermanentDelete && MessageBoxW(window_, L"Permanently delete the selected items? They will not go to the Recycle Bin.", L"Delete permanently", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return S_FALSE;
        return ShellOperations::remove(window_, items.Get(), command == PermanentDelete);
    }
    case Rename: return folderView_ ? folderView_->DoRename() : E_UNEXPECTED;
    case NewFolder: return newFolder();
    case NewText: return newText();
    case NewShortcut: return makeShortcut(false);
    case Properties: return showProperties(nullptr);
    case Sharing: return showProperties(L"Sharing");
    case Security: return showProperties(L"Security");
    case Open: return nativeVerb(L"open");
    case Edit: return nativeVerb(L"edit");
    case Print: return nativeVerb(L"print");
    case Pin: return nativeVerb(L"pintohome", true);
    case Undo: return ShellOperations::undo(window_);
    case Redo: return ShellOperations::redo(window_);
    case Zip: return archive(false);
    case Extract: return archive(true);
    case SelectAll: {
        if (!folderView_) return E_UNEXPECTED;
        int count = 0; auto hr = folderView_->ItemCount(SVGIO_ALLVIEW, &count);
        folderView_->SetRedraw(FALSE);
        for (int i = 0; SUCCEEDED(hr) && i < count; ++i) hr = folderView_->SelectItem(i, SVSI_SELECT);
        folderView_->SetRedraw(TRUE);
        return hr;
    }
    case SelectNone: return view_ ? view_->SelectItem(nullptr, SVSI_DESELECTOTHERS) : E_UNEXPECTED;
    case Invert: {
        if (!folderView_) return E_UNEXPECTED;
        int count = 0; auto hr = folderView_->ItemCount(SVGIO_ALLVIEW, &count);
        for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
            PITEMID_CHILD raw = nullptr;
            hr = folderView_->Item(i, &raw); Pidl pidl(raw);
            if (SUCCEEDED(hr)) {
                DWORD state = 0; folderView_->GetSelectionState(pidl.get(), &state);
                hr = folderView_->SelectItem(i, (state & SVSI_SELECT) ? SVSI_DESELECT : SVSI_SELECT);
            }
        }
        return hr;
    }
    case NavigationPane: preferences_.navigationPane = !preferences_.navigationPane; break;
    case PreviewPane: preferences_.previewPane = !preferences_.previewPane; preferences_.detailsPane = false; break;
    case DetailsPane: preferences_.detailsPane = !preferences_.detailsPane; preferences_.previewPane = false; break;
    case HiddenItems: preferences_.showHidden = !preferences_.showHidden; break;
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
    case Collapse: preferences_.ribbonCollapsed = !preferences_.ribbonCollapsed; rebuildRibbon(); return S_OK;
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
    case FileHistory: return headless_ ? E_ACCESSDENIED : shellExecute(window_, L"control.exe", L"/name Microsoft.FileHistory");
    case MapDrive: case DisconnectDrive:
        if (headless_) return E_ACCESSDENIED;
        { auto result = command == MapDrive ? WNetConnectionDialog(window_, RESOURCETYPE_DISK) : WNetDisconnectDialog(window_, RESOURCETYPE_DISK);
          return result == NO_ERROR ? S_OK : result == static_cast<DWORD>(-1) ? S_FALSE : HRESULT_FROM_WIN32(result); }
    case Terminal: {
        if (headless_) return E_ACCESSDENIED;
        auto hr = currentFolder(folder); if (FAILED(hr)) return hr;
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
    auto menu = CreatePopupMenu();
    auto add = [&](UINT id, const wchar_t* text, bool checked = false, bool disabled = false) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (disabled ? MF_GRAYED : 0), id, text);
    };
    auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    if (command == FileMenu) {
        add(NewWindow, L"Open new window\tCtrl+N"); add(Terminal, L"Open command prompt here"); separator();
        add(QuickAccess, L"Quick access"); add(ThisPC, L"This PC"); add(Desktop, L"Desktop");
        add(Documents, L"Documents"); add(Downloads, L"Downloads"); add(Pictures, L"Pictures");
        add(Music, L"Music"); add(Videos, L"Videos"); add(Libraries, L"Libraries");
        add(Network, L"Network"); add(RecycleBin, L"Recycle Bin"); separator();
        add(FolderOptions, L"Change folder and search options");
        add(Collapse, L"Minimize the ribbon\tCtrl+F1", preferences_.ribbonCollapsed); separator();
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
    }
    if (!anchor) anchor = command == FileMenu ? file_ : command == HistoryMenu ? nav_ : GetDlgItem(window_, command);
    RECT rect{};
    if (anchor) GetWindowRect(anchor, &rect);
    else { POINT point{}; GetCursorPos(&point); rect.left = point.x; rect.bottom = point.y; }
    const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (selected >= 4000 && selected - 4000 < history_.size()) {
        pendingHistory_ = selected - 4000;
        auto hr = browser_->BrowseToIDList(history_[pendingHistory_].get(), SBSP_ABSOLUTE);
        if (FAILED(hr)) pendingHistory_ = -1;
        showError(hr, L"Open history location");
    } else if (selected) showError(execute(selected), L"Command");
}
void ExplorerApp::showError(HRESULT hr, const wchar_t* action) {
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT) return;
    lastError_ = std::wstring(action) + L": " + hresultMessage(hr);
    setStatus(lastError_);
    if (!headless_) MessageBoxW(window_, lastError_.c_str(), L"Windows Explorer", MB_OK | MB_ICONERROR);
}
void ExplorerApp::persist() {
    if (headless_) return;
    RECT rect{}; GetWindowRect(window_, &rect);
    if (!IsIconic(window_)) {
        preferences_.windowWidth = rect.right - rect.left;
        preferences_.windowHeight = rect.bottom - rect.top;
    }
    savePreferences(preferencesPath(), preferences_);
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
        else { SetWindowTextW(window, L""); if (app->view_) app->view_->UIActivate(SVUIA_ACTIVATE_FOCUS); }
        return 0;
    }
    if (message == WM_KILLFOCUS && id == Address && app->addressEditing_) {
        app->addressEditing_ = false;
        ShowWindow(app->address_, SW_HIDE); ShowWindow(app->breadcrumbs_, SW_SHOW);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
LRESULT ExplorerApp::onMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {px(980), px(480)}; return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_);
        auto old = font_; font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        for (auto hwnd : {file_, tabs_, nav_, address_, breadcrumbs_, search_, status_}) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        rebuildRibbon(); updateBreadcrumbs(); layout(); if (old) DeleteObject(old);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED || lparam == 0) {
            auto command = LOWORD(wparam);
            if (command != Search && command != Address) showError(execute(command), L"Command");
            else if (command == Address && lparam == 0) editAddress();
        }
        return 0;
    case WM_NOTIFY: {
        auto notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->hwndFrom == tabs_ && notification->code == TCN_SELCHANGE) rebuildRibbon();
        return 0;
    }
    case WM_TIMER:
        if (!closing_) {
            if (archiveTask_.valid() && archiveTask_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const auto hr = archiveTask_.get();
                if (FAILED(hr)) showError(hr, archiveAction_.c_str());
                else if (view_) view_->Refresh();
            }
            updateStatus(); updateCommands();
        }
        return 0;
    case WM_APPCOMMAND:
        if (GET_APPCOMMAND_LPARAM(lparam) == APPCOMMAND_BROWSER_BACKWARD) { execute(Back); return TRUE; }
        if (GET_APPCOMMAND_LPARAM(lparam) == APPCOMMAND_BROWSER_FORWARD) { execute(Forward); return TRUE; }
        break;
    case DeferredUpdate: updateStatus(); updateCommands(); return 0;
    case DeferredView:
        if (folderView_ && !navigating_) {
            if (navigationCount_ == 1) setView(preferences_.view);
            folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, checkboxes_ ? FWF_CHECKSELECT : 0);
        }
        return 0;
    case WM_CLOSE: persist(); DestroyWindow(window_); return 0;
    case WM_DESTROY:
        closing_ = true; KillTimer(window_, 1); destroyBrowser();
        if (!headless_) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

int ExplorerApp::headlessSmoke(const std::filesystem::path& report) {
    struct Check { std::string name; bool passed; std::wstring detail; ULONGLONG milliseconds; };
    std::vector<Check> checks;
    auto check = [&](const char* name, bool passed, const std::wstring& detail = L"", ULONGLONG ms = 0) {
        checks.push_back({name, passed, detail, ms});
    };
    const auto started = GetTickCount64();
    const auto fixture = std::filesystem::temp_directory_path() / (L"WindowsExplorer-smoke-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(started));
    std::error_code filesystemError;
    try {
        check("host_stays_hidden", !IsWindowVisible(window_));
        check("initial_shell_navigation", pumpUntil([&] { return currentPidl_ && folderView_ && !navigating_; }, 10000));
        std::filesystem::create_directories(fixture / L"Subfolder");
        std::filesystem::create_directories(fixture / L"Unicode-\u65e5\u672c\u8a9e");
        for (int i = 0; i < 1000; ++i) {
            std::ofstream stream(fixture / (L"file-" + std::to_wstring(i) + L".txt"));
            stream << "headless fixture " << i;
        }
        std::ofstream(fixture / L"hidden.txt") << "hidden";
        SetFileAttributesW((fixture / L"hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
        std::ofstream(fixture / L"protected.txt") << "protected";
        SetFileAttributesW((fixture / L"protected.txt").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        const auto navigation = navigate(fixture.wstring());
        auto ready = pumpUntil([&] {
            int count = 0;
            return !navigating_ && folderView_ && currentLocation_ == fixture.wstring() &&
                   SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &count)) && count >= 1002;
        }, 15000);
        int count = -1;
        if (folderView_) folderView_->ItemCount(SVGIO_ALLVIEW, &count);
        check("enumerate_1000_files", SUCCEEDED(navigation) && ready && count == 1002,
              L"Visible items: " + std::to_wstring(count), lastNavigationMs_);
        check("breadcrumbs_and_history", breadcrumbsPidls_.size() >= 2 && historyIndex_ >= 1);
        check("native_view_interface", folderView_ && view_);
        if (ready) {
            for (int mode = 0; mode < 8; ++mode) {
                const auto hr = setView(static_cast<ViewMode>(mode));
                FOLDERVIEWMODE actual = FVM_AUTO; int size = 0;
                const auto read = folderView_->GetViewModeAndIconSize(&actual, &size);
                constexpr std::array<FOLDERVIEWMODE, 8> modes{FVM_ICON, FVM_ICON, FVM_ICON, FVM_SMALLICON, FVM_LIST, FVM_DETAILS, FVM_TILE, FVM_CONTENT};
                constexpr std::array<int, 8> sizes{256, 96, 48, 16, 16, 16, 48, 32};
                check(("view_" + std::to_string(mode)).c_str(), SUCCEEDED(hr) && SUCCEEDED(read) && actual == modes[mode] && size == sizes[mode],
                      std::wstring(ViewNames[mode]) + L": mode=" + std::to_wstring(actual) + L", size=" + std::to_wstring(size));
            }
            setView(ViewMode::Details);
            auto sortResult = setSort(PKEY_ItemNameDisplay);
            SORTCOLUMN sort{};
            check("sort_by_name", SUCCEEDED(sortResult) && SUCCEEDED(folderView_->GetSortColumns(&sort, 1)) &&
                  IsEqualPropertyKey(sort.propkey, PKEY_ItemNameDisplay) && sort.direction == SORT_ASCENDING);
            auto groupResult = setGroup(PKEY_ItemTypeText);
            PROPERTYKEY grouping{}; BOOL groupAscending = FALSE;
            check("group_by_type", SUCCEEDED(groupResult) && SUCCEEDED(folderView_->GetGroupBy(&grouping, &groupAscending)) &&
                  IsEqualPropertyKey(grouping, PKEY_ItemTypeText) && groupAscending);
            groupResult = setGroup(PKEY_Null);
            check("remove_grouping", SUCCEEDED(groupResult) && SUCCEEDED(folderView_->GetGroupBy(&grouping, &groupAscending)) && IsEqualPropertyKey(grouping, PKEY_Null));
            auto checkboxResult = execute(Checkboxes);
            DWORD currentFlags = 0;
            check("checkbox_selection_flag", SUCCEEDED(checkboxResult) && SUCCEEDED(folderView_->GetCurrentFolderFlags(&currentFlags)) && (currentFlags & FWF_CHECKSELECT));
            execute(Checkboxes);
            ComPtr<IColumnManager> columns;
            UINT columnCount = 0;
            check("native_details_columns", SUCCEEDED(folderView_.As(&columns)) && SUCCEEDED(columns->GetColumnCount(CM_ENUM_VISIBLE, &columnCount)) && columnCount >= 1,
                  std::to_wstring(columnCount));
            execute(SelectAll);
            ComPtr<IShellItemArray> selected;
            DWORD selectedCount = 0;
            if (SUCCEEDED(selection(selected))) selected->GetCount(&selectedCount);
            check("select_all", selectedCount == 1002, std::to_wstring(selectedCount));
            execute(SelectNone); selected.Reset(); selectedCount = 1;
            if (SUCCEEDED(selection(selected)) && selected) selected->GetCount(&selectedCount);
            else selectedCount = 0;
            check("select_none", selectedCount == 0);
            execute(Invert); selected.Reset(); selectedCount = 0;
            if (SUCCEEDED(selection(selected)) && selected) selected->GetCount(&selectedCount);
            check("invert_selection", selectedCount == 1002, std::to_wstring(selectedCount));
            execute(SelectNone);
            auto hr = navigate((fixture / L"Subfolder").wstring());
            check("navigate_subfolder", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && currentLocation_ == (fixture / L"Subfolder").wstring(); }, 5000));
            hr = execute(Back);
            check("back", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && currentLocation_ == fixture.wstring(); }, 5000));
            hr = execute(Forward);
            check("forward", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && currentLocation_ == (fixture / L"Subfolder").wstring(); }, 5000));
            hr = execute(Up);
            check("up", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && currentLocation_ == fixture.wstring(); }, 5000));
            execute(HiddenItems);
            ready = pumpUntil([&] { int total = 0; return !navigating_ && folderView_ && SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 1003; }, 10000);
            check("show_hidden_items", ready);
            execute(PreviewPane);
            ready = pumpUntil([&] { return !navigating_ && folderView_; }, 5000);
            EXPLORERPANESTATE pane = EPS_DONTCARE; GetPaneState(EP_PreviewPane, &pane);
            check("preview_pane_policy", ready && (pane & EPS_DEFAULT_ON));
            execute(DetailsPane);
            ready = pumpUntil([&] { return !navigating_ && folderView_; }, 5000);
            GetPaneState(EP_PreviewPane, &pane);
            check("panes_mutually_exclusive", ready && preferences_.detailsPane && !preferences_.previewPane && (pane & EPS_DEFAULT_OFF));
            check("invalid_location_returns_error", FAILED(navigate((fixture / L"does-not-exist").wstring())));
            auto previousCount = navigationCount_;
            SetWindowTextW(search_, L"filename:file-99");
            hr = execute(Search);
            const bool searchReady = SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && navigationCount_ > previousCount; }, 10000);
            check("search_folder_navigation", searchReady, currentLocation_);
            DWORD flags = 0; GetViewFlags(&flags);
            check("background_search_filtering_flag", searchReady && (flags & CDB2GVF_NOINCLUDEITEM) != 0);
            navigate(fixture.wstring());
            ready = pumpUntil([&] { int total = 0; return !navigating_ && folderView_ && currentLocation_ == fixture.wstring() && SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 1003; }, 5000);
            check("protected_files_remain_hidden", ready);
            execute(HiddenItems);
            ready = pumpUntil([&] { int total = 0; return !navigating_ && folderView_ && SUCCEEDED(folderView_->ItemCount(SVGIO_ALLVIEW, &total)) && total == 1002; }, 5000);
            check("hide_hidden_after_search", ready && !searchActive_);
            HWND nativeView = nullptr; view_->GetWindow(&nativeView);
            RECT expandedRect{}, collapsedRect{};
            GetWindowRect(nativeView, &expandedRect);
            execute(Collapse); GetWindowRect(nativeView, &collapsedRect);
            check("collapsed_ribbon_reclaims_space", expandedRect.top - collapsedRect.top >= px(80) && ribbonControls_.empty());
            execute(Collapse);
            hr = execute(ThisPC);
            check("this_pc_namespace", SUCCEEDED(hr) && pumpUntil([&] { return !navigating_ && currentLocation_.find(L"20D04FE0") != std::wstring::npos; }, 5000), currentLocation_);
        }
        check("no_visible_host_after_tests", !IsWindowVisible(window_));
        check("no_visible_process_windows_during_pump", !visibleWindowObserved);
        check("headless_mode_blocks_interactive_operations", execute(Delete) == E_ACCESSDENIED && execute(FolderOptions) == E_ACCESSDENIED && execute(Extensions) == E_ACCESSDENIED);
    } catch (const std::exception& error) {
        const std::string detail = error.what();
        check("unexpected_exception", false, std::wstring(detail.begin(), detail.end()));
    }
    destroyBrowser();
    std::filesystem::remove_all(fixture, filesystemError);
    check("fixture_cleanup", !filesystemError);
    auto failed = std::count_if(checks.begin(), checks.end(), [](const Check& value) { return !value.passed; });
    if (!report.parent_path().empty()) std::filesystem::create_directories(report.parent_path(), filesystemError);
    std::ofstream output(report);
    if (!output) return 9;
    output << "{\n  \"headless\": true,\n  \"passed\": " << (failed == 0 ? "true" : "false")
           << ",\n  \"checks\": " << checks.size() << ",\n  \"failed\": " << failed
           << ",\n  \"elapsed_ms\": " << GetTickCount64() - started << ",\n  \"results\": [\n";
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& result = checks[i];
        output << "    {\"name\": \"" << result.name << "\", \"passed\": " << (result.passed ? "true" : "false")
               << ", \"detail\": " << jsonString(result.detail) << ", \"milliseconds\": " << result.milliseconds << "}";
        output << (i + 1 < checks.size() ? ",\n" : "\n");
    }
    output << "  ]\n}\n";
    return failed == 0 ? 0 : 1;
}
}
