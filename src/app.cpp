#include "explorer/app.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/search.hpp"
#include "explorer/selection.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/search_presentation_store.hpp"
#include "explorer/input.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/theme.hpp"
#include "explorer/chrome.hpp"
#include "explorer/worker_sta.hpp"
#include "explorer/typed_address.hpp"
#include "explorer/ui_strings.hpp"
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
#include <functional>
#include <cstring>
#include <limits>
#include <new>

namespace explorer {
namespace {
constexpr wchar_t WindowClass[] = L"WindowsExplorer.Native.Window";
constexpr UINT DeferredUpdate = WM_APP + 1;
constexpr UINT DeferredView = WM_APP + 2;
constexpr UINT NamespaceResult = WM_APP + 4;
constexpr UINT TypedAddressFirst = 44000;
constexpr SFGAOF CommandSelectionAttributes = SFGAO_FILESYSTEM | SFGAO_HIDDEN | SFGAO_CANCOPY | SFGAO_CANMOVE |
    SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_HASPROPSHEET | SFGAO_FOLDER | SFGAO_LINK;
bool samePidlBytes(PCIDLIST_ABSOLUTE left, PCIDLIST_ABSOLUTE right) {
    if (!left || !right) return false;
    if (left == right) return true;
    const auto size = ILGetSize(left);
    return size == ILGetSize(right) && std::memcmp(left, right, size) == 0;
}
HRESULT readCommandSelection(IShellItemArray* selected,IFolderView2* view,DWORD* count,SFGAOF* attributes) {
    *count=0;*attributes=0;
    if(!selected) {
        int actual=-1;const auto hr=view?view->ItemCount(SVGIO_SELECTION,&actual):E_UNEXPECTED;
        return SUCCEEDED(hr)&&actual==0?S_OK:FAILED(hr)?hr:HRESULT_FROM_WIN32(ERROR_RETRY);
    }
    auto hr=selected->GetCount(count);
    if(SUCCEEDED(hr)&&*count)hr=selected->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),
        CommandSelectionAttributes,attributes);
    if(FAILED(hr))*attributes=0;
    return hr;
}
HRESULT commandSelectionIdentities(IShellItemArray* selected,DWORD count,std::vector<Pidl>* result) {
    try {
        std::vector<Pidl> ids;
        if(!count){*result=std::move(ids);return S_OK;}
        if(!selected)return E_INVALIDARG;
        ComPtr<IDataObject> data;auto hr=selected->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
        if(FAILED(hr))return hr;if(!data)return E_UNEXPECTED;
        const auto format=RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        if(!format)return HRESULT_FROM_WIN32(GetLastError());
        FORMATETC request{static_cast<CLIPFORMAT>(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
        struct Medium{STGMEDIUM value{};~Medium(){if(value.tymed)ReleaseStgMedium(&value);}}medium;
        hr=data->GetData(&request,&medium.value);if(FAILED(hr))return hr;
        if(medium.value.tymed!=TYMED_HGLOBAL||!medium.value.hGlobal)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const auto bytes=GlobalSize(medium.value.hGlobal);
        if(bytes<sizeof(UINT)||bytes>std::numeric_limits<UINT>::max())return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        struct Lock{HGLOBAL memory;const BYTE* value;~Lock(){if(value)GlobalUnlock(memory);}}locked{
            medium.value.hGlobal,static_cast<const BYTE*>(GlobalLock(medium.value.hGlobal))};
        if(!locked.value)return HRESULT_FROM_WIN32(GetLastError());
        UINT actual=0;std::memcpy(&actual,locked.value,sizeof(actual));
        // Bound the offset count before addition/multiplication, including
        // 32-bit hosts. A CIDA also has one parent offset after its children.
        const size_t maximumOffsets=(bytes-sizeof(UINT))/sizeof(UINT);
        if(actual!=count||actual>=maximumOffsets)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const size_t header=sizeof(UINT)+(static_cast<size_t>(actual)+1)*sizeof(UINT);
        const auto idAt=[&](size_t index,PCUIDLIST_RELATIVE* result)->HRESULT {
            UINT offset=0;std::memcpy(&offset,locked.value+sizeof(UINT)+index*sizeof(UINT),sizeof(offset));
            if(offset<header||offset>bytes-sizeof(USHORT))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            size_t cursor=offset;
            for(;;) {
                if(cursor>bytes-sizeof(USHORT))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                USHORT length=0;std::memcpy(&length,locked.value+cursor,sizeof(length));
                if(!length){*result=reinterpret_cast<PCUIDLIST_RELATIVE>(locked.value+offset);return S_OK;}
                if(length<sizeof(USHORT)||length>bytes-cursor)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                cursor+=length;
            }
        };
        PCUIDLIST_RELATIVE parent=nullptr;hr=idAt(0,&parent);if(FAILED(hr))return hr;
        ids.reserve(count);
        for(DWORD index=0;index<count;++index) {
            PCUIDLIST_RELATIVE child=nullptr;hr=idAt(static_cast<size_t>(index)+1,&child);if(FAILED(hr))return hr;
            Pidl id(ILCombine(parent,child));if(!id)return E_OUTOFMEMORY;ids.push_back(std::move(id));
        }
        DWORD retained=0;hr=selected->GetCount(&retained);if(FAILED(hr))return hr;
        if(retained!=count)return HRESULT_FROM_WIN32(ERROR_RETRY);
        *result=std::move(ids);return S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
bool usableOwnedControl(HWND host,HWND control) {
    if(!host||!control||!IsWindow(control)||!IsWindowEnabled(control)||
       (control!=host&&!IsChild(host,control)))return false;
    DWORD process=0;
    if(GetWindowThreadProcessId(control,&process)!=GetCurrentThreadId()||process!=GetCurrentProcessId())return false;
    for(auto parent=control;parent&&parent!=host;parent=GetParent(parent))
        if(!(GetWindowLongPtrW(parent,GWL_STYLE)&WS_VISIBLE)||!IsWindowEnabled(parent))return false;
    return true;
}
struct NativeFocusElement {
    ComPtr<IAccessible> object;
    LONG child=CHILDID_SELF;
    VARIANT argument() const {VARIANT value{};value.vt=VT_I4;value.lVal=child;return value;}
    LONG role() const {
        VARIANT value{};const auto hr=object?object->get_accRole(argument(),&value):E_POINTER;
        const auto result=SUCCEEDED(hr)&&value.vt==VT_I4?value.lVal:0;VariantClear(&value);return result;
    }
    bool state(LONG* result) const {
        VARIANT value{};const auto hr=object?object->get_accState(argument(),&value):E_POINTER;
        const bool read=SUCCEEDED(hr)&&value.vt==VT_I4;if(read)*result=value.lVal;VariantClear(&value);return read;
    }
    HWND window() const {HWND result=nullptr;if(object)WindowFromAccessibleObject(object.Get(),&result);return result;}
    bool usable(HWND host) const {
        LONG flags=0,left=0,top=0,width=0,height=0;const auto owner=window();RECT bounds{},visible{};
        if(!usableOwnedControl(host,owner)||!state(&flags)||
           (flags&(STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN|STATE_SYSTEM_UNAVAILABLE))||
           FAILED(object->accLocation(&left,&top,&width,&height,argument()))||width<=0||height<=0||
           !GetWindowRect(owner,&bounds))return false;
        const auto right=static_cast<LONGLONG>(left)+width,bottom=static_cast<LONGLONG>(top)+height;
        if(right>std::numeric_limits<LONG>::max()||bottom>std::numeric_limits<LONG>::max())return false;
        const RECT item{left,top,static_cast<LONG>(right),static_cast<LONG>(bottom)};
        return IntersectRect(&visible,&bounds,&item)!=FALSE;
    }
    bool focused() const {
        LONG flags=0;const auto owner=window(),focus=GetFocus();
        return state(&flags)&&(flags&STATE_SYSTEM_FOCUSED)&&owner==focus&&owner;
    }
    HRESULT focus(HWND host) const {
        if(!usable(host))return HRESULT_FROM_WIN32(ERROR_RETRY);
        // Native status radios omit FOCUSABLE until focused. Visibility,
        // geometry, ownership and the provider's actual focused readback
        // determine success; this never invokes or selects their commands.
        const auto hr=object->accSelect(SELFLAG_TAKEFOCUS,argument());
        return hr==S_OK?(usable(host)&&focused()?S_OK:E_FAIL):hr;
    }
};
NativeFocusElement accessibleChild(const NativeFocusElement& parent,LONG index) {
    NativeFocusElement result;if(!parent.object||parent.child!=CHILDID_SELF)return result;
    VARIANT value{};LONG obtained=0;
    if(SUCCEEDED(AccessibleChildren(parent.object.Get(),index,1,&value,&obtained))&&obtained==1) {
        if(value.vt==VT_DISPATCH&&value.pdispVal)value.pdispVal->QueryInterface(IID_PPV_ARGS(&result.object));
        else if(value.vt==VT_I4){result.object=parent.object;result.child=value.lVal;}
    }
    VariantClear(&value);return result;
}
LONG accessibleCount(const NativeFocusElement& parent) {
    LONG count=0;if(parent.object&&parent.child==CHILDID_SELF)parent.object->get_accChildCount(&count);return std::max(0L,count);
}
struct NativeFocusRegions {
    std::vector<NativeFocusElement> sorting,status;
    HWND tree=nullptr;
};
void collectStatusFocus(const NativeFocusElement& element,HWND host,bool inStatus,unsigned depth,
                        std::vector<NativeFocusElement>* result) {
    if(!element.object||depth>6||!element.usable(host))return;
    const auto role=element.role();inStatus=inStatus||role==ROLE_SYSTEM_STATUSBAR;
    if(inStatus&&role==ROLE_SYSTEM_RADIOBUTTON){result->push_back(element);return;}
    // The native frame's small layout/status hierarchy is separate from its
    // hosted folder and namespace controls. Do not enumerate file/tree rows.
    if(role==ROLE_SYSTEM_WINDOW||role==ROLE_SYSTEM_CLIENT||role==ROLE_SYSTEM_LIST||
       role==ROLE_SYSTEM_OUTLINE||role==ROLE_SYSTEM_LISTITEM||role==ROLE_SYSTEM_OUTLINEITEM)return;
    const auto count=std::min(32L,accessibleCount(element));
    for(LONG index=0;index<count;++index)collectStatusFocus(accessibleChild(element,index),host,inStatus,depth+1,result);
}
NativeFocusRegions nativeFocusRegions(HWND host,HWND view,bool details) {
    NativeFocusRegions result;
    struct Discovery{HWND host,view;bool details;NativeFocusRegions* result;}discovery{host,view,details,&result};
    EnumChildWindows(host,[](HWND child,LPARAM context)->BOOL {
        auto& search=*reinterpret_cast<Discovery*>(context);if(!usableOwnedControl(search.host,child))return TRUE;
        wchar_t type[64]{};GetClassNameW(child,type,64);
        const bool inView=search.view&&(child==search.view||IsChild(search.view,child));
        if(_wcsicmp(type,WC_TREEVIEWW)==0&&!inView) {
            for(auto parent=GetParent(child);parent&&parent!=search.host;parent=GetParent(parent)) {
                GetClassNameW(parent,type,64);
                if(_wcsicmp(type,L"NamespaceTreeControl")==0){search.result->tree=child;break;}
            }
            return TRUE;
        }
        if(_wcsicmp(type,L"DirectUIHWND")!=0&&_wcsicmp(type,WC_HEADERW)!=0)return TRUE;
        bool nativeFrame=false;
        for(auto parent=child;parent&&parent!=search.host;parent=GetParent(parent)) {
            GetClassNameW(parent,type,64);if(_wcsicmp(type,L"ExplorerBrowserControl")==0){nativeFrame=true;break;}
        }
        if(!nativeFrame)return TRUE;
        NativeFocusElement root;
        if(FAILED(AccessibleObjectFromWindow(child,static_cast<DWORD>(OBJID_CLIENT),IID_PPV_ARGS(&root.object))))return TRUE;
        if(inView&&search.details&&search.result->sorting.empty()) {
            // The native Details header is an immediate Items View container.
            // Inspect its controls only, never the complete selection/content.
            const auto count=std::min(32L,accessibleCount(root));
            for(LONG index=0;index<count&&search.result->sorting.empty();++index) {
                auto container=accessibleChild(root,index);const auto role=container.role();
                if(role==ROLE_SYSTEM_COLUMNHEADER&&container.usable(search.host))search.result->sorting.push_back(std::move(container));
                else if(role==ROLE_SYSTEM_LIST) {
                    const auto columns=std::min(128L,accessibleCount(container));
                    for(LONG column=0;column<columns;++column) {
                        auto button=accessibleChild(container,column);const auto buttonRole=button.role();
                        if((buttonRole==ROLE_SYSTEM_SPLITBUTTON||buttonRole==ROLE_SYSTEM_COLUMNHEADER)&&button.usable(search.host))
                            search.result->sorting.push_back(std::move(button));
                    }
                }
            }
        } else if(!inView&&search.result->status.empty())collectStatusFocus(root,search.host,false,0,&search.result->status);
        return TRUE;
    },reinterpret_cast<LPARAM>(&discovery));
    return result;
}
struct ToolbarFocusItem {HWND control=nullptr;int button=-1;};
void appendToolbarFocus(HWND host,HWND control,std::vector<ToolbarFocusItem>* result) {
    if(!usableOwnedControl(host,control))return;
    RECT client{};if(!GetClientRect(control,&client))return;
    const auto count=SendMessageW(control,TB_BUTTONCOUNT,0,0);
    for(int index=0;index<count;++index) {
        TBBUTTON button{};RECT bounds{},visible{};
        if(SendMessageW(control,TB_GETBUTTON,index,reinterpret_cast<LPARAM>(&button))&&
           (button.fsState&TBSTATE_ENABLED)&&!(button.fsState&TBSTATE_HIDDEN)&&!(button.fsStyle&BTNS_SEP)&&
           SendMessageW(control,TB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&bounds))&&IntersectRect(&visible,&client,&bounds))
            result->push_back({control,index});
    }
}
HRESULT focusToolbarItem(HWND host,const ToolbarFocusItem& item) {
    if(!usableOwnedControl(host,item.control))return HRESULT_FROM_WIN32(ERROR_RETRY);
    SetFocus(item.control);
    if(item.button>=0)SendMessageW(item.control,TB_SETHOTITEM,item.button,0);
    return GetFocus()==item.control&&(item.button<0||SendMessageW(item.control,TB_GETHOTITEM,0,0)==item.button)?S_OK:E_FAIL;
}
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
    if(searchActive_&&location==currentLocation_) {
        SearchWindowContext context;
        const auto captured=currentSearchWindowContext(&context);
        return FAILED(captured)?captured:launchSearchWindow(executable,context);
    }
    return shellExecute(window_,executable.c_str(),(L"--path "+quoteArgument(location)).c_str());
}

HRESULT ExplorerApp::currentSearchWindowContext(SearchWindowContext* result) {
    if(!result)return E_POINTER;
    if(closing_||navigating_||!searchActive_||!searchScope_||!view_||!folderView_)return E_UNEXPECTED;
    try {
        const auto nativeView=view_;const auto nativeFolderView=folderView_;
        const auto revision=searchInteractionRevision_;const auto navigation=navigationCount_;
        const auto query=activeQuery_;
        Pidl location(currentPidl_?ILCloneFull(currentPidl_.get()):nullptr);
        if(!location)return E_OUTOFMEMORY;
        auto hr=captureActiveSearchPresentation();if(FAILED(hr))return hr;
        SearchWindowContext candidate;candidate.query=query;candidate.recursive=searchRecursive_;
        hr=SHCreateItemFromIDList(searchScope_.get(),IID_PPV_ARGS(&candidate.primaryScope));
        if(FAILED(hr))return hr;
        candidate.scopes=searchScopes_;
        if(!candidate.scopes)hr=SHCreateShellItemArrayFromShellItem(candidate.primaryScope.Get(),IID_PPV_ARGS(&candidate.scopes));
        if(FAILED(hr))return hr;
        if(searchWindowOrigin_)candidate.closeOrigin=searchWindowOrigin_;
        else if(liveSearchOrigin_)hr=SHCreateItemFromIDList(liveSearchOrigin_.get(),IID_PPV_ARGS(&candidate.closeOrigin));
        else candidate.closeOrigin=candidate.primaryScope;
        if(FAILED(hr))return hr;
        candidate.rules=searchScopeRules_;candidate.presentation=searchPresentation_;candidate.fileProperties=searchFileProperties_;
        if(closing_||navigating_||!searchActive_||view_.Get()!=nativeView.Get()||folderView_.Get()!=nativeFolderView.Get()||
           searchInteractionRevision_!=revision||navigationCount_!=navigation||activeQuery_!=query||
           !samePidlBytes(location.get(),currentPidl_.get()))return HRESULT_FROM_WIN32(ERROR_RETRY);
        *result=std::move(candidate);return S_OK;
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
}

HRESULT ExplorerApp::prepareSearchWindowContext(const SearchWindowContext& context) {
    if(window_||browser_||closing_||preparedSearchWindowTarget_||!searchLocations_.empty())return E_UNEXPECTED;
    try {
        // Validate the same complete packet accepted by child startup. This
        // also keeps direct private-host verification on the production path.
        std::vector<BYTE> packet;auto hr=encodeSearchWindowContext(context,&packet);if(FAILED(hr))return hr;
        SearchWindowContext checked;if(FAILED(hr=decodeSearchWindowContext(packet,&checked)))return hr;
        ComPtr<IShellItem> results;
        hr=checked.rules.empty()?createSearchFolderForScopes(checked.query,checked.scopes.Get(),&results,checked.recursive):
            createSearchFolderForScopeRules(checked.query,checked.rules,&results);
        if(FAILED(hr))return hr;
        PIDLIST_ABSOLUTE raw=nullptr;hr=SHGetIDListFromObject(results.Get(),&raw);Pidl target(raw);
        if(FAILED(hr))return hr;if(!target)return E_UNEXPECTED;
        Pidl prepared(ILCloneFull(target.get()));if(!prepared)return E_OUTOFMEMORY;
        raw=nullptr;hr=SHGetIDListFromObject(checked.primaryScope.Get(),&raw);Pidl scope(raw);
        if(FAILED(hr))return hr;if(!scope)return E_UNEXPECTED;
        SearchLocation entry{};entry.location=std::move(target);entry.scope=std::move(scope);
        entry.query=checked.query;entry.recursive=checked.recursive;entry.base=checked.query;
        entry.scopes=std::move(checked.scopes);entry.scopeRules=std::move(checked.rules);
        entry.presentation=std::move(checked.presentation);entry.fileProperties=std::move(checked.fileProperties);
        entry.importedPresentation=true;entry.windowOrigin=checked.closeOrigin?checked.closeOrigin:checked.primaryScope;
        searchLocations_.push_back(std::move(entry));preparedSearchWindowTarget_=std::move(prepared);return S_OK;
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
}

HRESULT ExplorerApp::OnDefaultCommand(IShellView* source) {
    if(!source)return S_FALSE;
    if(newWindowMode_&&headless_)return E_ACCESSDENIED;
    if(navigating_||closing_||!view_)return S_FALSE;
    // Preserve the view's modifier-specific default command (for example Alt
    // properties); this policy applies to an ordinary folder activation.
    if(!headless_&&((GetKeyState(VK_CONTROL)|GetKeyState(VK_SHIFT)|GetKeyState(VK_MENU))&0x8000))return S_FALSE;
    ComPtr<IUnknown> activeIdentity,sourceIdentity;
    if(FAILED(view_.As(&activeIdentity))||FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceIdentity)))||
       activeIdentity.Get()!=sourceIdentity.Get())return S_FALSE;
    ComPtr<IShellItemArray> selected;
    DWORD count=0;
    if(FAILED(selection(selected))||!selected||FAILED(selected->GetCount(&count))||count!=1)return S_FALSE;
    ComPtr<IShellItem> item;SFGAOF attributes=0;
    if(FAILED(selected->GetItemAt(0,&item))||FAILED(item->GetAttributes(SFGAO_FOLDER,&attributes))||
       !(attributes&SFGAO_FOLDER))return S_FALSE;
    if(!newWindowMode_)return openLongSavedSearch(item.Get());
    const auto location=itemName(item.Get(),SIGDN_DESKTOPABSOLUTEPARSING);
    const auto hr=openNewWindow(location);
    return SUCCEEDED(hr)?S_OK:hr;
}

HRESULT ExplorerApp::prepareHeadlessVisual(const VisualScene& scene) {
    const auto desktop=PrivateDesktop::current();
    if(!headless_||!desktop||!desktop->ready()||FAILED(desktop->verifyIsolation()))return E_ACCESSDENIED;
    if(window_||browser_||view_||folderView_)return E_UNEXPECTED;
    // Pane visibility is read during creation of the original native view.
    // Avoid replacing a view that may already own asynchronous native work.
    preferences_.detailsPane=scene.details;
    preferences_.previewPane=false;
    return S_OK;
}

HRESULT ExplorerApp::create(const std::wstring& location) {
    auto directionStatus = loadThreadUiDirection(&uiDirection_);
    if (FAILED(directionStatus)) return directionStatus;
    bool rightToLeft = uiDirection_.rightToLeft;
    if (headlessDirectionOverride_) {
        const auto desktop = PrivateDesktop::current();
        if (!headless_ || window_ || !desktop || !desktop->ready() || FAILED(desktop->verifyIsolation())) return E_ACCESSDENIED;
        rightToLeft = *headlessDirectionOverride_;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance_; wc.lpszClassName = WindowClass; wc.lpfnWndProc = windowProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
    // Mirroring already implies RTL reading. RTLREADING reverses that
    // direction again when combined with LAYOUTRTL.
    const DWORD extended = WS_EX_CONTROLPARENT | (rightToLeft ? WS_EX_LAYOUTRTL : 0);
    window_ = CreateWindowExW(extended, WindowClass, L"Windows Explorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
    initializeSearchRefinementChoices();
    auto hr = createControls();
    if (FAILED(hr)) { DestroyWindow(window_); return hr; }
    hr = createBrowser();
    if (FAILED(hr)) { DestroyWindow(window_); return hr; }
    layout();
    hr = preparedSearchWindowTarget_?browser_->BrowseToIDList(preparedSearchWindowTarget_.get(),SBSP_ABSOLUTE):
        navigate(location.empty() ? (preferences_.useWindowsStartup ? windowsDefaultStartupLocation() : preferences_.startupLocation) : location);
    preparedSearchWindowTarget_.reset();
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
HRESULT ExplorerApp::recreateBrowser(UINT toggleCommand) {
    if(toggleCommand!=NavigationPane&&toggleCommand!=PreviewPane&&toggleCommand!=DetailsPane&&toggleCommand!=HiddenItems)
        return E_INVALIDARG;
    if(closing_||navigating_)return HRESULT_FROM_WIN32(ERROR_BUSY);
    CommandRefreshScope refresh(*this);
    Pidl location(currentPidl_ ? ILCloneFull(currentPidl_.get()) : nullptr);
    if(currentPidl_&&!location)return E_OUTOFMEMORY;
    const auto originalView=view_;
    const auto originalFolderView=folderView_;
    const auto originalNavigation=navigationCount_;
    const auto originalRevision=searchInteractionRevision_;
    if(searchActive_) {
        const auto captured=captureActiveSearchPresentation();
        if(FAILED(captured))return captured;
    }
    if(closing_||navigating_||view_.Get()!=originalView.Get()||folderView_.Get()!=originalFolderView.Get()||
       navigationCount_!=originalNavigation||searchInteractionRevision_!=originalRevision||
       (location?!samePidlBytes(location.get(),currentPidl_.get()):currentPidl_!=nullptr))return HRESULT_FROM_WIN32(ERROR_RETRY);
    const auto previousNavigation=preferences_.navigationPane;
    const auto previousPreview=preferences_.previewPane;
    const auto previousDetails=preferences_.detailsPane;
    const auto previousHidden=preferences_.showHidden;
    const auto restorePreferences=[&] {
        preferences_.navigationPane=previousNavigation;preferences_.previewPane=previousPreview;
        preferences_.detailsPane=previousDetails;preferences_.showHidden=previousHidden;
    };
    SHELLSTATE previousSettings{};
    const auto restoreHiddenPolicy=[&] {
        if(toggleCommand!=HiddenItems||headless_)return;
        SHELLSTATE current{};SHGetSetSettings(&current,SSF_SHOWALLOBJECTS,FALSE);
        if(static_cast<bool>(current.fShowAllObjects)==!previousHidden) {
            SHGetSetSettings(&previousSettings,SSF_SHOWALLOBJECTS,TRUE);
            SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
        }
    };
    switch(toggleCommand) {
    case NavigationPane: preferences_.navigationPane=!previousNavigation;break;
    case PreviewPane: preferences_.previewPane=!previousPreview;preferences_.detailsPane=false;break;
    case DetailsPane: preferences_.detailsPane=!previousDetails;preferences_.previewPane=false;break;
    case HiddenItems:
        preferences_.showHidden=!previousHidden;
        if(!headless_) {
            SHGetSetSettings(&previousSettings,SSF_SHOWALLOBJECTS,FALSE);
            auto settings=previousSettings;settings.fShowAllObjects=preferences_.showHidden;
            SHGetSetSettings(&settings,SSF_SHOWALLOBJECTS,TRUE);
            SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
        }
        break;
    }
    if(closing_||navigating_||view_.Get()!=originalView.Get()||folderView_.Get()!=originalFolderView.Get()||
       navigationCount_!=originalNavigation||searchInteractionRevision_!=originalRevision||
       (location?!samePidlBytes(location.get(),currentPidl_.get()):currentPidl_!=nullptr)) {
        restorePreferences();
        restoreHiddenPolicy();
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    }
    destroyBrowser();
    const auto hr = createBrowser();
    layout();
    if (FAILED(hr)) {
        restorePreferences();
        restoreHiddenPolicy();
        return hr;
    }
    const auto browsed=location ? browser_->BrowseToIDList(location.get(), SBSP_ABSOLUTE) :
        navigate(preferences_.useWindowsStartup ? windowsDefaultStartupLocation() : preferences_.startupLocation);
    if(FAILED(browsed)){restorePreferences();restoreHiddenPolicy();}
    rebuildRibbon();
    return browsed;
}
HRESULT ExplorerApp::navigate(const std::wstring& location, bool typedAddress) {
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
        const auto savedOpen=openLongSavedSearch(item.Get(),typedAddress?location:std::wstring{});
        if(savedOpen!=S_FALSE)return savedOpen;
    }
    pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
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
HRESULT ExplorerApp::openLongSavedSearch(IShellItem* item, const std::wstring& typedAddress) {
    if(!item||!browser_||!itemHasFastType(item,L".search-ms"))return S_FALSE;
    const auto filesystem=itemName(item,SIGDN_FILESYSPATH);
    if(filesystem.empty())return S_FALSE;
    // The Shell may report a short alias even though the saved file's real
    // name exceeds MAX_PATH. Resolve that existing name through Win32 without
    // substituting another file or changing any long-path machine setting.
    auto input=filesystem;
    if(!input.starts_with(L"\\\\?\\"))input=input.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+input.substr(2):L"\\\\?\\"+input;
    const auto needed=GetLongPathNameW(input.c_str(),nullptr,0);
    if(!needed||needed>32768)return S_FALSE;
    std::wstring path(needed,L'\0');
    const auto copied=GetLongPathNameW(input.c_str(),path.data(),needed);
    if(!copied||copied>=needed)return S_FALSE;
    path.resize(copied);
    auto shellPath=path;
    if(shellPath.starts_with(L"\\\\?\\UNC\\"))shellPath=L"\\\\"+shellPath.substr(8);
    else if(shellPath.starts_with(L"\\\\?\\"))shellPath.erase(0,4);
    if(shellPath.size()<MAX_PATH||_wcsicmp(std::filesystem::path(shellPath).extension().c_str(),L".search-ms")!=0)return S_FALSE;
    const auto revision=searchInteractionRevision_;
    ComPtr<IShellFolder> nativeFolder;
    const auto native=item->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&nativeFolder));
    if(native!=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))return S_FALSE;
    SavedSearchMetadata metadata;
    auto hr=readSavedSearch(std::filesystem::path(path),&metadata);
    // Unsupported external shapes retain their original native failure;
    // there is no partially imported context or changed query meaning.
    if(FAILED(hr))return native;
    auto presentation=metadata.presentation;
    if(!headless_) {
        auto actual=presentation.value_or(SearchViewPresentation{});
        hr=loadSearchPresentationCompanion(std::filesystem::path(path),searchPresentationDirectory(),&actual);
        if(FAILED(hr))return hr;
        if(hr==S_OK)presentation=std::move(actual);
    }
    ComPtr<IShellItem> results;
    if(FAILED(hr=createSearchFolderForScopeRules(metadata.query,metadata.scopeRules,&results)))return hr;
    ComPtr<IShellFolder> restoredFolder;
    if(FAILED(hr=results->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&restoredFolder))))return hr;
    PIDLIST_ABSOLUTE raw=nullptr;
    if(FAILED(hr=SHGetIDListFromObject(results.Get(),&raw)))return hr;
    Pidl location(raw);
    raw=nullptr;
    if(FAILED(hr=SHGetIDListFromObject(metadata.scope.Get(),&raw)))return hr;
    Pidl scope(raw);
    Pidl addressTarget(typedAddress.empty()?nullptr:ILCloneFull(location.get()));
    if(!location||!scope||(!typedAddress.empty()&&!addressTarget))return E_OUTOFMEMORY;
    if(closing_||searchInteractionRevision_!=revision)return E_ABORT;
    if(navigating_||pendingDirectSearchTarget_||pendingLiveSearchTarget_)return HRESULT_FROM_WIN32(ERROR_BUSY);
    SearchLocation imported{std::move(location),std::move(scope),metadata.query,metadata.recursive,
        metadata.query,{},metadata.scopes,std::move(metadata.scopeRules),std::move(presentation),std::move(metadata.fileProperties),false,false,true};
    Pidl nativeTarget(ILCloneFull(imported.location.get()));
    const bool alreadyCurrent=currentPidl_&&ILIsEqual(currentPidl_.get(),imported.location.get());
    Pidl reusedScope(alreadyCurrent?ILCloneFull(imported.scope.get()):nullptr);
    if(!nativeTarget||(alreadyCurrent&&!reusedScope))return E_OUTOFMEMORY;
    // All import/factory validation has completed. The ordinary browser
    // callbacks now commit only the genuinely completed native results view.
    cancelLiveSearch();
    const auto installedRevision=searchInteractionRevision_;
    pendingHistory_=-1;
    pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
    if(!typedAddress.empty()&&typedAddressHistoryAllowed_) {
        pendingTypedAddress_=typedAddress;pendingTypedAddressTarget_=std::move(addressTarget);
    }
    std::optional<SearchLocation> previous;
    const auto existing=std::find_if(searchLocations_.rbegin(),searchLocations_.rend(),
        [&](const SearchLocation& entry){return ILIsEqual(entry.location.get(),nativeTarget.get());});
    PCIDLIST_ABSOLUTE registered=nullptr;
    if(existing!=searchLocations_.rend()) {
        // A factory PIDL identifies its query/scopes, not the saved source
        // file's metadata. Replace this context tentatively rather than
        // spending another cache entry for each repeated file open.
        previous.emplace(std::move(*existing));*existing=std::move(imported);registered=existing->location.get();
    } else {searchLocations_.push_back(std::move(imported));registered=searchLocations_.back().location.get();}
    hr=alreadyCurrent?S_OK:browser_->BrowseToObject(results.Get(),SBSP_ABSOLUTE);
    if(FAILED(hr)) {
        const auto failed=std::find_if(searchLocations_.begin(),searchLocations_.end(),
            [&](const SearchLocation& entry){return entry.location.get()==registered;});
        if(failed!=searchLocations_.end()) {
            if(previous)*failed=std::move(*previous);
            else searchLocations_.erase(failed);
        }
        pendingTypedAddress_.clear();pendingTypedAddressTarget_.reset();
        return hr;
    }
    const auto retained=std::find_if(searchLocations_.begin(),searchLocations_.end(),
        [&](const SearchLocation& entry){return entry.location.get()==registered;});
    if(alreadyCurrent&&!navigating_&&searchInteractionRevision_==installedRevision&&retained!=searchLocations_.end()&&
       currentPidl_&&ILIsEqual(currentPidl_.get(),nativeTarget.get())) {
        // Reusing the same actual native PIDL need not produce callbacks.
        // Apply only the newly imported file metadata and presentation to
        // that verified view; history and navigation counts stay untouched.
        auto& context=*retained;
        searchActive_=searchBackground_=pendingSearchActive_=pendingSearchBackground_=true;
        searchScope_=std::move(reusedScope);searchScopes_=context.scopes;searchScopeRules_=context.scopeRules;
        activeQuery_=context.query;searchBase_=context.base;searchFilters_=context.filters;searchRecursive_=context.recursive;
        searchPresentation_=context.presentation;searchFileProperties_=context.fileProperties;
        context.completedLocation.reset(ILCloneFull(currentPidl_.get()));
        rememberSearchCacheHistory(currentPidl_.get());
        context.importedPresentation=false;
        searchPresentationStatus_=S_OK;searchPresentationPending_=searchPresentation_.has_value();
        setSearchText(activeQuery_);refreshSearchRefinements();updateContextTabs();PostMessageW(window_,DeferredView,0,0);scheduleDeferredUpdate();
        rememberAddressNavigation(currentPidl_.get());
    }
    const auto newest=std::find_if(searchLocations_.rbegin(),searchLocations_.rend(),
        [&](const SearchLocation& entry){return ILIsEqual(entry.location.get(),nativeTarget.get());});
    if(newest!=searchLocations_.rend()&&newest->location.get()==registered)
        std::erase_if(searchLocations_,[&](const SearchLocation& entry) {
            return entry.location.get()!=registered&&ILIsEqual(entry.location.get(),nativeTarget.get());
        });
    pruneSearchCaches();
    return S_OK;
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
    // Owned test callback at the real in-flight browser boundary. Normal
    // windows never install it; it cannot fabricate navigation/provider state.
    if(headless_&&expectedLive&&pendingLiveSearch_&&pendingLiveSearch_->kind==LiveSearchKind::Query&&
       headlessLiveNavigationProbe_) {
        auto probe=std::move(headlessLiveNavigationProbe_);headlessLiveNavigationProbe_={};probe();
    }
    return S_OK;
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
    searchWindowOrigin_.Reset();
    searchPresentation_.reset();
    searchFileProperties_.reset();
    searchPresentationPending_=false;
    searchPresentationStatus_=S_OK;
    std::wstring completedExplicitQuery;
    auto completedQueryRevision=directRevision;
    if (searchActive_) {
        // The browser can complete with a native PIDL that is canonically
        // equal to the factory PIDL but has different serialized bytes.
        // Preserve that supplied identity without changing the factory key.
        search->completedLocation.reset(ILCloneFull(pidl));
        searchScope_.reset(ILCloneFull(search->scope.get()));
        searchScopes_=search->scopes;
        searchScopeRules_=search->scopeRules;
        activeQuery_ = search->query;
        searchRecursive_ = search->recursive;
        searchBase_ = search->base;
        searchFilters_ = search->filters;
        searchPresentation_=search->presentation;
        searchFileProperties_=search->fileProperties;
        searchWindowOrigin_=search->windowOrigin;
        if(search->rememberOnComplete&&!search->remembered) {
            // Consume an obsolete direct request too, so visiting this query
            // later through Back does not commit a superseded interaction.
            search->remembered=true;
            if(directNavigation)completedExplicitQuery=search->query;
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
                searchFileProperties_=std::move(metadata.fileProperties);
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
    const bool importedPresentation=searchActive_&&search!=searchLocations_.rend()&&search->importedPresentation;
    if(importedPresentation)search->importedPresentation=false;
    if(searchActive_&&!importedPresentation) {
        const auto remembered=std::find_if(searchPresentationLocations_.rbegin(),searchPresentationLocations_.rend(),
            [&](const SearchPresentationLocation& entry){return ILIsEqual(entry.location.get(),pidl);});
        if(remembered!=searchPresentationLocations_.rend()) {
            remembered->completedLocation.reset(ILCloneFull(pidl));
            searchPresentation_=remembered->presentation;
        }
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
    rememberSearchCacheHistory(pidl);
    updateFrameTitle();
    SetWindowTextW(address_, currentLocation_.c_str());
    UiString searchCue;
    if (SUCCEEDED(formatUiString(UiText::SearchCue, currentName_, &searchCue)))
        SendMessageW(search_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(searchCue.text.c_str()));
    if(liveNavigation)completeLiveSearchNavigation(pidl,S_OK);
    else if(directNavigation) {
        // The factory may pump a real edit after the chosen command has
        // started. Its completion must leave that newer literal and request.
        if(searchInteractionRevision_==directRevision) {
            setSearchText(searchActive_?activeQuery_:L"");
            completedQueryRevision=searchInteractionRevision_;
        } else {
            completedExplicitQuery.clear();
            if(pendingDirectSearchTarget_&&pendingDirectSearchRevision_==directRevision&&
               ILIsEqual(pendingDirectSearchTarget_.get(),pidl))pendingDirectSearchTarget_.reset();
        }
    } else if(!pendingLiveSearch_&&!pendingDirectSearchTarget_&&!liveSearchPolicy_.waiting()) {
        // A provider can report another completed view while an edit request
        // is queued or its expected navigation is still pending. Ordinary
        // navigation canceled that work in OnNavigationPending; an unrelated
        // completion must not cancel or overwrite a newer accepted edit.
        setSearchText(searchActive_?activeQuery_:L"");
    }
    updateBreadcrumbs();
    refreshSearchRefinements();
    updateContextTabs();
    if(!completedExplicitQuery.empty()&&searchInteractionRevision_==completedQueryRevision)rememberQuery(completedExplicitQuery);
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
    return S_OK;
}
HRESULT ExplorerApp::OnStateChange(IShellView* source, ULONG change) {
    const auto event = std::min<size_t>(change, headlessCurrentViewStateEvents_.size() - 1);
    const auto stale = [&] {
        if(headless_)++headlessStaleViewStateEvents_[event];
        return S_OK;
    };
    if(closing_||!source||!view_)return stale();
    // A replaced native view can finish asynchronous selection/state work
    // after navigation. Only its own current view may invalidate this host's
    // selection snapshot; otherwise it cancels unrelated native state jobs.
    // Retain the candidate while QI runs, and reject a reentrant replacement.
    ComPtr<IShellView> active = view_;
    if(source!=active.Get()) {
        ComPtr<IUnknown> activeIdentity, sourceIdentity;
        if(FAILED(active.As(&activeIdentity))||FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceIdentity)))||
           activeIdentity.Get()!=sourceIdentity.Get())return stale();
    }
    if(active.Get()!=view_.Get())return stale();
    if(headless_)++headlessCurrentViewStateEvents_[event];
    // Ribbon popup focus does not change the original view selection. Retain
    // its native command snapshot until selection, rename or state changes.
    if(change==CDBOSC_SETFOCUS||change==CDBOSC_KILLFOCUS)return S_OK;
    // Selection notifications may arrive repeatedly while the native view's
    // selection is already settled. Compare its full original identities in
    // deferred readback before cancelling native command-state work. Rename
    // and checkbox/state notifications also invalidate metadata for the same
    // identities, so they retain their unconditional namespace refresh.
    selectionStateDirty_ = true;
    if(change!=CDBOSC_SELCHANGE)namespaceDirty_=true;
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
    bool rightToLeft = false;
    const auto directionRead = windowUiDirection(window_, &rightToLeft);
    if (FAILED(directionRead)) return directionRead;
    UiString navigationName, addressToolbarName, backName, forwardName, historyName, upName;
    UiString addressName, searchName, refreshName, collapseName, collapseTip;
    for (const auto& [key, text] : std::array<std::pair<UiText, UiString*>, 11>{{
        {UiText::NavigationButtons, &navigationName}, {UiText::AddressToolbar, &addressToolbarName},
        {UiText::Back, &backName}, {UiText::Forward, &forwardName}, {UiText::RecentLocations, &historyName},
        {UiText::UpTooltip, &upName}, {UiText::AddressBar, &addressName}, {UiText::SearchBox, &searchName},
        {UiText::Refresh, &refreshName}, {UiText::MinimiseRibbon, &collapseName},
        {UiText::MinimiseRibbonTooltip, &collapseTip}}}) {
        const auto loaded = loadUiString(key, text);
        if (FAILED(loaded)) return loaded;
    }
    auto control = [&](const wchar_t* type, const wchar_t* label, DWORD style, UINT id) {
        auto hwnd = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                   window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance_, nullptr);
        if (hwnd) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return hwnd;
    };
    nav_ = control(TOOLBARCLASSNAMEW, navigationName.text.c_str(), WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 901);
    if (!nav_) return HRESULT_FROM_WIN32(GetLastError());
    SendMessageW(nav_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    auto toolbarButton = [&](HWND toolbar, UINT id, const std::wstring& label) -> HRESULT {
        // TB_ADDSTRING copies its double-NUL-terminated input. The toolbar
        // retains its own caption after these local resource results expire.
        auto strings = label;
        strings.push_back(L'\0');
        const auto stringIndex = SendMessageW(toolbar, TB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(strings.c_str()));
        if (stringIndex < 0) return E_OUTOFMEMORY;
        TBBUTTON button{}; button.iBitmap = I_IMAGENONE; button.idCommand = id;
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_BUTTON;
        button.iString = stringIndex;
        return SendMessageW(toolbar, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button)) ? S_OK : E_FAIL;
    };
    for (const auto& [command, text] : std::array<std::pair<UINT, const UiString*>, 4>{{
        {Back, &backName}, {Forward, &forwardName}, {HistoryMenu, &historyName}, {Up, &upName}}}) {
        const auto added = toolbarButton(nav_, command, text->text);
        if (FAILED(added)) return added;
    }
    breadcrumbs_ = control(TOOLBARCLASSNAMEW, addressToolbarName.text.c_str(), WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS |
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
    addressActions_ = control(TOOLBARCLASSNAMEW, addressToolbarName.text.c_str(), WS_TABSTOP | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
        CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER, 905);
    if (!nav_ || !breadcrumbs_ || !address_ || !search_ || !addressActions_) return HRESULT_FROM_WIN32(GetLastError());
    SendMessageW(addressActions_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    const auto refreshAdded = toolbarButton(addressActions_, Refresh, refreshName.text);
    if (FAILED(refreshAdded)) return refreshAdded;
    // Toolbar tooltips are owned popup windows, so native child inheritance
    // cannot supply their layout. Keep their direction tied to this host.
    if (rightToLeft) for (const auto toolbar : {nav_, breadcrumbs_, addressActions_}) {
        const auto tooltip = reinterpret_cast<HWND>(SendMessageW(toolbar, TB_GETTOOLTIPS, 0, 0));
        if (!tooltip) continue;
        const auto style = GetWindowLongPtrW(tooltip, GWL_EXSTYLE);
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(tooltip, GWL_EXSTYLE, (style | WS_EX_LAYOUTRTL) & ~static_cast<LONG_PTR>(WS_EX_RTLREADING)) && GetLastError())
            return HRESULT_FROM_WIN32(GetLastError());
    }
    ComPtr<IAccPropServices> accessible;
    if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&accessible)))) {
        accessible->SetHwndPropStr(address_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, addressName.text.c_str());
        accessible->SetHwndPropStr(search_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, searchName.text.c_str());
    }
    const auto chromeStatus = applyChrome(nav_, breadcrumbs_, address_, search_, addressActions_);
    if (FAILED(chromeStatus)) return chromeStatus;
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
    ribbonCollapse_=control(L"BUTTON",collapseName.text.c_str(),BS_OWNERDRAW|BS_FLAT|WS_TABSTOP,Collapse);
    if(!ribbonCollapse_)return HRESULT_FROM_WIN32(GetLastError());
    const auto collapseTheme=applyRibbonCollapseButton(ribbonCollapse_);
    if(FAILED(collapseTheme))return collapseTheme;
    const DWORD tooltipExtended = WS_EX_TOPMOST | (rightToLeft ? WS_EX_LAYOUTRTL : 0);
    ribbonCollapseTooltip_=CreateWindowExW(tooltipExtended,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance_,nullptr);
    if(ribbonCollapseTooltip_) {
        ribbonCollapseTip_=collapseTip.text;
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
    ComPtr<IShellView> targetView=view_;
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
    std::optional<std::vector<Pidl>> targetIdentities;
    DWORD targetSelectionCount=0;SFGAOF targetSelectionAttributes=0;
    if(SUCCEEDED(readCommandSelection(target.selection.Get(),folderView_.Get(),&targetSelectionCount,&targetSelectionAttributes))) {
        std::vector<Pidl> identities;
        if(SUCCEEDED(commandSelectionIdentities(target.selection.Get(),targetSelectionCount,&identities)))
            targetIdentities=std::move(identities);
    }
    selectionKinds_={};
    const auto kindsStarted=commandTimingNow();
    if(target.selection)namespaceSelectionKinds(target.selection.Get(),&selectionKinds_);
    const auto kindsCompleted=commandTimingNow();
    if(headless_)commandTimings_.selectionKindsMs+=kindsCompleted-kindsStarted;
    target.site = targetView;
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
    commandSelectionIdentities_=std::move(targetIdentities);
    commandSelectionAttributes_=targetSelectionAttributes;
    commandSelectionView_=targetView.Get();
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
    const auto activeView=view_;HWND nativeView=nullptr;
    if(activeView)activeView->GetWindow(&nativeView);
    FOLDERVIEWMODE mode=FVM_AUTO;int iconSize=0;
    const bool details=folderView_&&SUCCEEDED(folderView_->GetViewModeAndIconSize(&mode,&iconSize))&&mode==FVM_DETAILS;
    const auto regions=nativeFocusRegions(window_,nativeView,details);
    std::vector<ToolbarFocusItem> toolbar;
    appendToolbarFocus(window_,nav_,&toolbar);
    if(addressEditing_&&usableOwnedControl(window_,address_))toolbar.push_back({address_,-1});
    else appendToolbarFocus(window_,breadcrumbs_,&toolbar);
    appendToolbarFocus(window_,addressActions_,&toolbar);
    if(usableOwnedControl(window_,search_))toolbar.push_back({search_,-1});
    if(activeView.Get()!=view_.Get()||closing_)return HRESULT_FROM_WIN32(ERROR_RETRY);
    const auto current=currentFocusRegion();
    if(activeView.Get()!=view_.Get()||closing_)return HRESULT_FROM_WIN32(ERROR_RETRY);
    const FocusAvailability available{usableOwnedControl(window_,nativeView),!regions.sorting.empty(),!regions.status.empty(),
                                      !toolbar.empty(),preferences_.navigationPane&&usableOwnedControl(window_,regions.tree)};
    const auto next=cycleFocusRegion(current,backwards,available);
    if (!next) return S_FALSE;
    HRESULT focused=E_UNEXPECTED;
    switch (*next) {
    case FocusRegion::FolderView:focused=activeView->UIActivate(SVUIA_ACTIVATE_FOCUS);break;
    case FocusRegion::Sorting:focused=regions.sorting.front().focus(window_);break;
    case FocusRegion::Status:focused=regions.status.front().focus(window_);break;
    case FocusRegion::Toolbar:focused=focusToolbarItem(window_,toolbar.front());break;
    case FocusRegion::Navigation:SetFocus(regions.tree);focused=GetFocus()==regions.tree?S_OK:E_FAIL;break;
    }
    return activeView.Get()==view_.Get()&&!closing_?focused:HRESULT_FROM_WIN32(ERROR_RETRY);
}
std::optional<FocusRegion> ExplorerApp::currentFocusRegion() const {
    const auto activeView=view_;HWND nativeView=nullptr;if(activeView)activeView->GetWindow(&nativeView);
    FOLDERVIEWMODE mode=FVM_AUTO;int size=0;
    const bool details=folderView_&&SUCCEEDED(folderView_->GetViewModeAndIconSize(&mode,&size))&&mode==FVM_DETAILS;
    const auto regions=nativeFocusRegions(window_,nativeView,details);
    if(activeView.Get()!=view_.Get()||closing_)return std::nullopt;
    if(std::any_of(regions.sorting.begin(),regions.sorting.end(),[](const NativeFocusElement& item){return item.focused();}))return FocusRegion::Sorting;
    if(std::any_of(regions.status.begin(),regions.status.end(),[](const NativeFocusElement& item){return item.focused();}))return FocusRegion::Status;
    const auto focus=GetFocus();auto within=[focus](HWND parent){return parent&&(focus==parent||IsChild(parent,focus));};
    if(within(nativeView))return FocusRegion::FolderView;
    if(within(regions.tree))return FocusRegion::Navigation;
    if(within(nav_)||within(breadcrumbs_)||within(address_)||within(addressActions_)||within(search_))return FocusRegion::Toolbar;
    return std::nullopt;
}
HRESULT ExplorerApp::cycleToolbarFocus(bool backwards) {
    std::vector<ToolbarFocusItem> items;
    appendToolbarFocus(window_,nav_,&items);
    if(addressEditing_&&usableOwnedControl(window_,address_))items.push_back({address_,-1});
    else appendToolbarFocus(window_,breadcrumbs_,&items);
    appendToolbarFocus(window_,addressActions_,&items);
    if(usableOwnedControl(window_,search_))items.push_back({search_,-1});
    if(items.empty())return S_FALSE;
    const auto focus=GetFocus();
    const auto current=std::find_if(items.begin(),items.end(),[focus](const ToolbarFocusItem& item){
        return item.control==focus&&(item.button<0||SendMessageW(item.control,TB_GETHOTITEM,0,0)==item.button);
    });
    size_t index=backwards?items.size()-1:0;
    if(current!=items.end()) {
        const auto previous=static_cast<size_t>(current-items.begin());
        index=backwards?(previous+items.size()-1)%items.size():(previous+1)%items.size();
    }
    return focusToolbarItem(window_,items[index]);
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

HRESULT ExplorerApp::captureActiveSearchPresentation() {
    if(!searchActive_||!folderView_)return E_UNEXPECTED;
    try {
        const auto nativeView=view_;
        const auto nativeFolderView=folderView_;
        const auto generation=navigationCount_;
        const auto revision=searchInteractionRevision_;
        Pidl identity(currentPidl_?ILCloneFull(currentPidl_.get()):nullptr);
        if(currentPidl_&&!identity)return E_OUTOFMEMORY;
        SearchViewPresentation actual;
        const auto hr=captureSearchViewPresentation(nativeFolderView.Get(),&actual);
        if(FAILED(hr))return hr;
        if(closing_||navigating_||!searchActive_||view_.Get()!=nativeView.Get()||folderView_.Get()!=nativeFolderView.Get()||
           navigationCount_!=generation||searchInteractionRevision_!=revision||
           (identity?!samePidlBytes(identity.get(),currentPidl_.get()):currentPidl_!=nullptr))
            return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(!identity){searchPresentation_=std::move(actual);return S_OK;}
        // Completion/history aliases are already native-canonical identities.
        // Byte comparisons here cannot dispatch COM while cache iterators live.
        const auto matches=[&](const auto& entry) {
            return samePidlBytes(entry.location.get(),identity.get())||
                samePidlBytes(entry.completedLocation.get(),identity.get())||
                samePidlBytes(entry.historyLocation.get(),identity.get());
        };
        if(std::none_of(searchPresentationLocations_.begin(),searchPresentationLocations_.end(),matches))
            pruneSearchCaches(99); // Reserve the new current entry without pruning after publication.
        const auto location=std::find_if(searchLocations_.rbegin(),searchLocations_.rend(),matches);
        const auto remembered=std::find_if(searchPresentationLocations_.rbegin(),searchPresentationLocations_.rend(),matches);
        const auto contextIndex=location==searchLocations_.rend()?searchLocations_.size():
            static_cast<size_t>(std::distance(searchLocations_.begin(),location.base())-1);
        const auto rememberedIndex=remembered==searchPresentationLocations_.rend()?searchPresentationLocations_.size():
            static_cast<size_t>(std::distance(searchPresentationLocations_.begin(),remembered.base())-1);
        Pidl contextCompleted(ILCloneFull(identity.get())),rememberedCompleted(ILCloneFull(identity.get()));
        const bool historyValid=historyIndex_>=0&&historyIndex_<static_cast<int>(history_.size());
        Pidl contextHistory(historyValid?ILCloneFull(history_[historyIndex_].get()):nullptr);
        Pidl rememberedHistory(historyValid?ILCloneFull(history_[historyIndex_].get()):nullptr);
        if(!contextCompleted||!rememberedCompleted||(historyValid&&(!contextHistory||!rememberedHistory)))return E_OUTOFMEMORY;
        auto contextPresentation=actual;
        auto rememberedPresentation=actual;
        if(rememberedIndex==searchPresentationLocations_.size())searchPresentationLocations_.reserve(searchPresentationLocations_.size()+1);
        // Presentation copies/allocations precede publication. Failed native
        // read/staging does not publish new presentation metadata.
        searchPresentation_=std::move(actual);
        if(contextIndex<searchLocations_.size()) {
            auto& context=searchLocations_[contextIndex];
            context.completedLocation=std::move(contextCompleted);context.historyLocation=std::move(contextHistory);
            context.presentation=std::move(contextPresentation);
        }
        if(rememberedIndex<searchPresentationLocations_.size()) {
            auto& cached=searchPresentationLocations_[rememberedIndex];
            cached.completedLocation=std::move(rememberedCompleted);cached.historyLocation=std::move(rememberedHistory);
            cached.presentation=std::move(rememberedPresentation);
        } else {
            searchPresentationLocations_.push_back({std::move(identity),std::move(rememberedPresentation),
                std::move(rememberedCompleted),std::move(rememberedHistory)});
        }
        return S_OK;
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
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
    const auto* properties=searchFileProperties_?&*searchFileProperties_:nullptr;
    hr=searchScopeRules_.empty()?explorer::saveSearchForScopes(activeQuery_,scopes.Get(),searchRecursive_,std::filesystem::path(path),SearchSaveMode::UserConfirmed,&native,properties):
        explorer::saveSearchForScopeRules(activeQuery_,searchScopeRules_,std::filesystem::path(path),SearchSaveMode::UserConfirmed,&native,properties);
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
    const auto delay=navigating_||pendingDirectSearchTarget_||pendingLiveSearchTarget_||
        commandRefreshActive_||liveSearchDispatchActive_?100ULL:
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
    if(navigating_||pendingDirectSearchTarget_||pendingLiveSearchTarget_||
       commandRefreshActive_||liveSearchDispatchActive_) {scheduleLiveSearch();return S_OK;}
    LiveSearchDispatchScope dispatch(*this);
    const auto request=liveSearchPolicy_.takeReady(GetTickCount64(),true);
    if(!request) {scheduleLiveSearch();return S_FALSE;}
    KillTimer(window_,6);
    if(request->kind==LiveSearchKind::ReturnToOrigin) {
        if(!liveSearchOrigin_&&searchWindowOrigin_) {
            PIDLIST_ABSOLUTE raw=nullptr;auto hr=SHGetIDListFromObject(searchWindowOrigin_.Get(),&raw);Pidl origin(raw);
            if(SUCCEEDED(hr)&&!origin)hr=E_UNEXPECTED;
            if(FAILED(hr)){liveSearchPolicy_.finish(*request,hr);return hr;}
            liveSearchOrigin_=std::move(origin);
        }
        if(!liveSearchOrigin_&&searchActive_&&searchScope_)liveSearchOrigin_.reset(ILCloneFull(searchScope_.get()));
        if(!liveSearchOrigin_&&searchBackground_&&historyIndex_>0)
            liveSearchOrigin_.reset(ILCloneFull(history_[historyIndex_-1].get()));
        if(!liveSearchOrigin_) {
            liveSearchPolicy_.finish(*request,S_OK);setSearchText(L"",false);return S_OK;
        }
        pendingLiveSearch_=*request;pendingLiveSearchTarget_.reset(ILCloneFull(liveSearchOrigin_.get()));
        if(!pendingLiveSearchTarget_) {liveSearchPolicy_.finish(*request,E_OUTOFMEMORY);pendingLiveSearch_.reset();return E_OUTOFMEMORY;}
        if(currentPidl_&&ILIsEqual(currentPidl_.get(),liveSearchOrigin_.get())) {
            // No browser request has been issued. A cancelled/deferred edit
            // can already be at its actual origin, so finish without queuing
            // a redundant native navigation behind the next user intent.
            completeLiveSearchNavigation(currentPidl_.get(),S_OK);return S_OK;
        }
        pendingHistory_=-1;
        for(size_t index=0;index<history_.size();++index)
            if(ILIsEqual(history_[index].get(),liveSearchOrigin_.get()))pendingHistory_=static_cast<int>(index);
        liveSearchStatus_=browser_?browser_->BrowseToIDList(liveSearchOrigin_.get(),SBSP_ABSOLUTE):E_UNEXPECTED;
    } else {
        if(!currentPidl_) {liveSearchPolicy_.finish(*request,E_UNEXPECTED);return E_UNEXPECTED;}
        // Editing an already active results view continues its existing Back
        // history slot, including a restored or repeated native query.
        if(liveSearchHistoryIndex_<0&&searchBackground_&&historyIndex_>=0&&
           historyIndex_<static_cast<int>(history_.size())&&ILIsEqual(history_[historyIndex_].get(),currentPidl_.get()))
            liveSearchHistoryIndex_=historyIndex_;
        if(!liveSearchOrigin_) {
            if(searchWindowOrigin_) {
                PIDLIST_ABSOLUTE raw=nullptr;auto hr=SHGetIDListFromObject(searchWindowOrigin_.Get(),&raw);Pidl origin(raw);
                if(SUCCEEDED(hr)&&!origin)hr=E_UNEXPECTED;
                if(FAILED(hr)){liveSearchPolicy_.finish(*request,hr);return hr;}
                liveSearchOrigin_=std::move(origin);
            }
            else if(searchActive_&&searchScope_)liveSearchOrigin_.reset(ILCloneFull(searchScope_.get()));
            else if(searchBackground_&&historyIndex_>0)liveSearchOrigin_.reset(ILCloneFull(history_[historyIndex_-1].get()));
            else liveSearchOrigin_.reset(ILCloneFull(currentPidl_.get()));
        }
        if(!liveSearchOrigin_) {liveSearchPolicy_.finish(*request,E_OUTOFMEMORY);return E_OUTOFMEMORY;}
        liveSearchStatus_=startSearch(request->literal,searchRecursive_,{},L"",&*request);
    }
    const auto result=liveSearchStatus_;
    bool retryScheduled=false;
    if(FAILED(result)) {
        if(result==HRESULT_FROM_WIN32(ERROR_BUSY))retryScheduled=liveSearchPolicy_.retry(*request,GetTickCount64());
        else liveSearchPolicy_.finish(*request,result);
        pendingLiveSearch_.reset();pendingLiveSearchTarget_.reset();scheduleLiveSearch();
    }
    // Retain the native busy status for diagnostics, while an accepted Enter
    // retry remains queued rather than opening an error dialog.
    return retryScheduled?S_OK:result;
}
void ExplorerApp::rememberSearchCacheHistory(PCIDLIST_ABSOLUTE location) {
    if (!location || historyIndex_ < 0 || historyIndex_ >= static_cast<int>(history_.size())) return;
    const auto remember = [&](auto& cache) {
        const auto entry = std::find_if(cache.rbegin(), cache.rend(), [&](const auto& value) {
            return samePidlBytes(value.completedLocation.get(), location);
        });
        if (entry != cache.rend()) entry->historyLocation.reset(ILCloneFull(history_[historyIndex_].get()));
    };
    remember(searchLocations_);
    remember(searchPresentationLocations_);
}

void ExplorerApp::pruneSearchCaches(size_t maximumCachedLocations) {
    struct Fingerprint {
        PCIDLIST_ABSOLUTE location = nullptr;
        UINT size = 0;
        std::uint64_t hash = 0;
    };
    const auto fingerprint = [](PCIDLIST_ABSOLUTE location) {
        Fingerprint result;
        if (!location) return result;
        result.location = location;
        result.size = ILGetSize(location);
        result.hash = 14695981039346656037ULL;
        const auto bytes = reinterpret_cast<const BYTE*>(location);
        for (UINT index = 0; index < result.size; ++index) {
            result.hash ^= bytes[index];
            result.hash *= 1099511628211ULL;
        }
        return result;
    };
    // Only identical serialized native identities share a fingerprint.
    // Navigation records actual completed/travel identities separately from
    // a factory PIDL; no provider comparisons are needed during pruning.
    // Hashes shortlist candidates; full bytes make collisions harmless.
    const auto sameLocation = [](const Fingerprint& left, const Fingerprint& right) {
        return left.location && right.location && (left.location == right.location ||
            (left.size == right.size && left.hash == right.hash &&
             std::memcmp(left.location, right.location, left.size) == 0));
    };
    const PCIDLIST_ABSOLUTE targets[] = {
        currentPidl_.get(), pendingPidl_.get(), pendingDirectSearchTarget_.get(),
        pendingLiveSearchTarget_.get(), liveSearchOrigin_.get(), pendingTypedAddressTarget_.get(),
        selectionDestination_.get()
    };
    std::vector<Fingerprint> pins;
    pins.reserve(history_.size() + std::size(targets));
    for (const auto& location : history_) if (location) pins.push_back(fingerprint(location.get()));
    for (const auto location : targets) if (location) pins.push_back(fingerprint(location));
    using Identities = std::array<Fingerprint, 3>;
    const auto sameEntry = [&](const Identities& left, const Identities& right) {
        return std::any_of(left.begin(), left.end(), [&](const Fingerprint& location) {
            return std::any_of(right.begin(), right.end(), [&](const Fingerprint& target) {
                return sameLocation(location, target);
            });
        });
    };
    const auto pinned = [&](const Identities& entry, const Identities& newest) {
        return sameEntry(entry, newest) || std::any_of(entry.begin(), entry.end(), [&](const Fingerprint& location) {
            return std::any_of(pins.begin(), pins.end(), [&](const Fingerprint& target) {
                return sameLocation(location, target);
            });
        });
    };
    const auto prune = [&](auto& cache) {
        if (cache.empty()) return;
        std::vector<Identities> identities;
        identities.reserve(cache.size());
        for (const auto& entry : cache) identities.push_back({fingerprint(entry.location.get()),
            fingerprint(entry.completedLocation.get()), fingerprint(entry.historyLocation.get())});
        std::vector<bool> retained(cache.size(), true);
        auto retainedCount = cache.size();
        // Completion resolves the newest matching context. Keep that same
        // meaning when a query is revisited or a saved file is reimported.
        for (size_t index = 0; index < cache.size(); ++index) {
            const auto newer = identities.begin() + static_cast<std::ptrdiff_t>(index + 1);
            if (std::any_of(newer, identities.end(),
                [&](const Identities& entry) { return sameEntry(identities[index], entry); })) {
                retained[index] = false; --retainedCount;
            }
        }
        // The newest factory context is registered before its pending target.
        // Travel history and in-flight navigation must remain reconstructible
        // even after many live queries replace a single history slot.
        const auto& newest = identities.back();
        for (size_t index = 0; index < cache.size() && retainedCount > maximumCachedLocations; ++index) {
            if (retained[index] && !pinned(identities[index], newest)) {
                retained[index] = false; --retainedCount;
            }
        }
        // If every entry is pinned, the 100-item travel history plus the fixed
        // in-flight targets bound the cache; dropping one would lose metadata.
        size_t destination = 0;
        for (size_t index = 0; index < cache.size(); ++index) if (retained[index]) {
            if (destination != index) cache[destination] = std::move(cache[index]);
            ++destination;
        }
        cache.resize(destination);
    };
    prune(searchLocations_);
    prune(searchPresentationLocations_);
}

HRESULT ExplorerApp::startSearch(const std::wstring& requested, bool recursive,
                                std::optional<size_t> category, const std::wstring& filter,
                                const LiveSearchRequest* liveRequest) {
    LiveSearchDispatchScope factoryDispatch(*this);
    const bool nativeBrowsePending = navigating_ || pendingDirectSearchTarget_ || pendingLiveSearchTarget_;
    auto query = trim(requested);
    if (query.empty() && !category) return S_FALSE;
    std::wstring refinedQuery;
    if (category) {
        if (*category >= searchFilters_.size()) return E_INVALIDARG;
        const auto inspectionRevision = searchInteractionRevision_;
        const auto inspectionNavigation = navigationCount_;
        const auto inspectionQuery = activeQuery_;
        std::shared_ptr<NativeSearchRefinements> refinement = searchActive_ && query == searchRefinementQuery_ ? searchRefinements_ : nullptr;
        HRESULT refinementRead = S_OK;
        if (!refinement && searchActive_ && query == searchRefinementQuery_ && FAILED(searchRefinementStatus_))
            return searchRefinementStatus_;
        if (!refinement) refinementRead = NativeSearchRefinements::inspect(query, &refinement);
        if (SUCCEEDED(refinementRead)) refinementRead = refinement->replace(static_cast<SearchRefinementCategory>(*category), filter, &refinedQuery);
        if (FAILED(refinementRead)) return refinementRead;
        if (closing_ || searchInteractionRevision_ != inspectionRevision || navigationCount_ != inspectionNavigation || activeQuery_ != inspectionQuery)
            return S_FALSE;
    }
    // A rejected native-category rewrite leaves the committed query and any
    // newer live intent intact. Cancellation begins only after its preflight.
    if(!liveRequest)cancelLiveSearch();
    const auto directRevision=searchInteractionRevision_;
    if(searchActive_&&folderView_) {
        const auto captured=captureActiveSearchPresentation();
        if(FAILED(captured))return captured;
    }
    const auto presentation=searchActive_?searchPresentation_:std::optional<SearchViewPresentation>{};
    const auto fileProperties=searchActive_?searchFileProperties_:std::optional<SearchFileProperties>{};
    const auto windowOrigin=searchWindowOrigin_;
    auto base = query;
    std::array<std::wstring, 3> filters;
    if (searchActive_ && query == activeQuery_) { base = searchBase_; filters = searchFilters_; }
    if (category) {
        filters[*category] = filter;
        query = std::move(refinedQuery);
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
    if(!nativeBrowsePending&&!navigating_&&searchActive_&&recursive==searchRecursive_&&
       currentPidl_&&ILIsEqual(location.get(),currentPidl_.get())) {
        // The actual native view already represents this exact query/scope.
        // A redundant BrowseToObject can be accepted before the browser posts
        // OnNavigationPending, briefly rejecting a following Close/Back as
        // busy despite the unchanged current view. Retain its context/history.
        if(query!=activeQuery_||base!=searchBase_||filters!=searchFilters_) {
            Pidl reusedScope(ILCloneFull(raw)),completed(ILCloneFull(currentPidl_.get()));
            const bool hasHistory=historyIndex_>=0&&historyIndex_<static_cast<int>(history_.size());
            Pidl travel(hasHistory?ILCloneFull(history_[static_cast<size_t>(historyIndex_)].get()):nullptr);
            if(!reusedScope||!completed||(hasHistory&&!travel)) {CoTaskMemFree(raw);return E_OUTOFMEMORY;}
            // The native condition/scope identity is unchanged, but a new
            // textual intent owns its base/refinements and future Back metadata.
            // Preserve the original factory identity plus actual view aliases.
            searchLocations_.push_back({std::move(location),Pidl(raw),query,recursive,base,filters,
                scopes,rules,presentation,fileProperties,!liveRequest,!liveRequest,false,std::move(completed),std::move(travel)});
            searchLocations_.back().windowOrigin=windowOrigin;
            raw=nullptr;
            searchScope_=std::move(reusedScope);searchScopes_=scopes;searchScopeRules_=std::move(rules);
            activeQuery_=query;searchBase_=base;searchFilters_=filters;searchRecursive_=recursive;
            searchPresentation_=presentation;searchFileProperties_=fileProperties;
            pruneSearchCaches();
        }
        CoTaskMemFree(raw);
        if(liveRequest) {
            pendingLiveSearch_=*liveRequest;
            pendingLiveSearchTarget_.reset(ILCloneFull(currentPidl_.get()));
            if(!pendingLiveSearchTarget_) {pendingLiveSearch_.reset();return E_OUTOFMEMORY;}
            completeLiveSearchNavigation(currentPidl_.get(),S_OK);
        } else {setSearchText(query);rememberQuery(query);}
        refreshSearchRefinements();
        return S_OK;
    }
    Pidl directTarget;
    if(!liveRequest) {
        directTarget.reset(ILCloneFull(location.get()));
        if(!directTarget) {CoTaskMemFree(raw);return E_OUTOFMEMORY;}
    }
    searchLocations_.push_back({std::move(location), Pidl(raw), query, recursive, base, filters,scopes,std::move(rules),presentation,fileProperties,!liveRequest,false});
    searchLocations_.back().windowOrigin=windowOrigin;
    pruneSearchCaches();
    if(liveRequest) {
        pendingLiveSearch_=*liveRequest;
        pendingLiveSearchTarget_.reset(ILCloneFull(searchLocations_.back().location.get()));
        if(!pendingLiveSearchTarget_) {pendingLiveSearch_.reset();return E_OUTOFMEMORY;}
    } else {
        pendingDirectSearchTarget_=std::move(directTarget);
        pendingDirectSearchRevision_=directRevision;
    }
    if(liveRequest&&headless_&&headlessLiveBrowseProbe_) {
        auto probe=std::move(headlessLiveBrowseProbe_);headlessLiveBrowseProbe_={};hr=probe();
    } else hr = browser_->BrowseToObject(results.Get(), SBSP_ABSOLUTE);
    if(FAILED(hr)&&!liveRequest&&pendingDirectSearchRevision_==directRevision)pendingDirectSearchTarget_.reset();
    return hr;
}

void ExplorerApp::updateBreadcrumbs() {
    if (!currentPidl_) return;
    const auto generation = navigationCount_;
    Pidl location(ILCloneFull(currentPidl_.get()));
    Pidl cursor(location ? ILCloneFull(location.get()) : nullptr);
    if (!cursor) { showError(E_OUTOFMEMORY, L"Read breadcrumb ancestors"); return; }
    std::vector<Pidl> ancestors;
    for (;;) {
        Pidl ancestor(ILCloneFull(cursor.get()));
        if (!ancestor) { showError(E_OUTOFMEMORY, L"Read breadcrumb ancestors"); return; }
        ancestors.push_back(std::move(ancestor));
        if (ILIsEmpty(cursor.get())) break;
        if (!ILRemoveLastID(cursor.get())) { showError(E_FAIL, L"Read breadcrumb ancestors"); return; }
    }
    std::reverse(ancestors.begin(), ancestors.end());
    std::vector<std::wstring> labels;
    labels.reserve(ancestors.size());
    for (const auto& ancestor : ancestors) {
        auto label = pidlName(ancestor.get(), SIGDN_NORMALDISPLAY);
        if (label.empty()) label = pidlName(ancestor.get(), SIGDN_DESKTOPABSOLUTEPARSING);
        if (label.empty()) { showError(E_FAIL, L"Read breadcrumb name"); return; }
        labels.push_back(std::move(label));
    }
    // Native name/icon providers may dispatch another real navigation. Never
    // publish the older model over that completed destination.
    if (closing_ || generation != navigationCount_ || !samePidlBytes(location.get(), currentPidl_.get())) return;
    SHFILEINFOW currentIcon{};
    const auto images=ImageList_Create(px(16),px(16),ILC_COLOR32|ILC_MASK,1,0);
    if(images) {
        if(SHGetFileInfoW(reinterpret_cast<LPCWSTR>(location.get()),0,&currentIcon,sizeof(currentIcon),
            SHGFI_PIDL|SHGFI_ICON|SHGFI_SMALLICON)&&currentIcon.hIcon) {
            ImageList_AddIcon(images,currentIcon.hIcon);DestroyIcon(currentIcon.hIcon);
        } else if(folderIcon_)ImageList_AddIcon(images,folderIcon_);
        if (closing_ || generation != navigationCount_ || !samePidlBytes(location.get(), currentPidl_.get())) {
            ImageList_Destroy(images); return;
        }
        SendMessageW(breadcrumbs_,TB_SETIMAGELIST,0,reinterpret_cast<LPARAM>(images));
        if(breadcrumbImages_)ImageList_Destroy(breadcrumbImages_);
        breadcrumbImages_=images;
    }
    breadcrumbsPidls_ = std::move(ancestors);
    breadcrumbLabels_ = std::move(labels);
    breadcrumbButtons_.clear(); breadcrumbHiddenAncestors_.clear();
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
    DWORD currentFlags = 0;
    if (folderView_ && SUCCEEDED(folderView_->GetCurrentFolderFlags(&currentFlags)))
        checkboxes_ = (currentFlags & FWF_CHECKSELECT) != 0;
    SORTCOLUMN sorted{};
    if (folderView_ && SUCCEEDED(folderView_->GetSortColumns(&sorted, 1))) ascending_ = sorted.direction == SORT_ASCENDING;
    const auto clipboardSequence = GetClipboardSequenceNumber();
    if (clipboardSequence != clipboardSequence_) {
        clipboardSequence_ = clipboardSequence;
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
        selection(selected);
        const auto selectedRead=readCommandSelection(selected.Get(),folderView_.Get(),&selectionCount_,&selectionAttributes_);
        bool equivalent=false;
        if(!namespaceDirty_&&SUCCEEDED(selectedRead)&&commandSelectionIdentities_&&commandSelectionView_==view_.Get()&&
           selectionCount_==commandSelectionIdentities_->size()&&selectionAttributes_==commandSelectionAttributes_) {
            std::vector<Pidl> identities;
            if(SUCCEEDED(commandSelectionIdentities(selected.Get(),selectionCount_,&identities))) {
                equivalent=identities.size()==commandSelectionIdentities_->size()&&
                    std::equal(identities.begin(),identities.end(),commandSelectionIdentities_->begin(),
                        [](const Pidl& current,const Pidl& previous){return ILIsEqual(current.get(),previous.get())!=FALSE;});
            }else if(headless_)++headlessUncertainSelectionRefreshes_;
        }
        const auto attributesCompleted=commandTimingNow();
        if(headless_)commandTimings_.selectionCountAttributesMs+=attributesCompleted-selectionStarted;
        const auto statusCompleted=commandTimingNow();
        if(headless_)commandTimings_.selectionStatusMs+=statusCompleted-attributesCompleted;
        if(equivalent) {if(headless_)++headlessEquivalentSelectionRefreshes_;}
        else namespaceDirty_ = true;
        if(headless_)commandTimings_.selectionHostEligibilityMs+=commandTimingNow()-statusCompleted;
    }
    if (namespaceDirty_ && !navigating_) updateContextTabsImpl();
    const auto ribbonStarted=commandTimingNow();
    bool minimized = false;
    if (SUCCEEDED(ribbon_.minimized(minimized))) preferences_.ribbonCollapsed = minimized;
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
HRESULT ExplorerApp::browseHistory(int offset) {
    auto index = historyIndex_ + offset;
    if (index < 0 || index >= static_cast<int>(history_.size())) return S_FALSE;
    Pidl target(ILCloneFull(history_[index].get()));
    return target ? browseHistoryLocation(target.get(), index) : E_OUTOFMEMORY;
}
HRESULT ExplorerApp::browseHistoryLocation(PCIDLIST_ABSOLUTE location, int originalIndex) {
    if (!browser_ || !location) return E_UNEXPECTED;
    pendingHistory_ = originalIndex >= 0 && originalIndex < static_cast<int>(history_.size()) &&
        ILIsEqual(history_[originalIndex].get(), location) ? originalIndex : -1;
    cancelLiveSearch();
    const auto hr = browser_->BrowseToIDList(location, SBSP_ABSOLUTE);
    if (FAILED(hr)) pendingHistory_ = -1;
    else if (!navigating_ && currentPidl_ && ILIsEqual(currentPidl_.get(), location) && pendingHistory_ >= 0) {
        // Reusing the same folder can complete without a navigation callback.
        historyIndex_ = pendingHistory_; pendingHistory_ = -1; updateCommands();
    }
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
    // An existing empty codec is the user's explicit app-history deletion.
    // Import the read-only native list once, when our codec is absent.
    if(addressHistoryStatus_!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)&&
       addressHistoryStatus_!=HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))return;
    std::vector<std::wstring> imported;
    const auto nativeRead=loadWindowsTypedAddresses(&imported);
    if(SUCCEEDED(nativeRead)) {
        const auto merge=mergeTypedAddressHistory(typedAddresses_,imported);
        addressHistoryStatus_=FAILED(merge)?merge:nativeRead;
    } else addressHistoryStatus_=nativeRead;
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
HRESULT ExplorerApp::addressContextSnapshot(AddressContextSnapshot* result) const {
    if (!result) return E_POINTER;
    if (!currentPidl_) return E_UNEXPECTED;
    AddressContextSnapshot snapshot;
    snapshot.location.reset(ILCloneFull(currentPidl_.get()));
    if (!snapshot.location) return E_OUTOFMEMORY;
    snapshot.text = pidlName(snapshot.location.get(), SIGDN_DESKTOPABSOLUTEEDITING);
    if (snapshot.text.empty()) snapshot.text = pidlName(snapshot.location.get(), SIGDN_DESKTOPABSOLUTEPARSING);
    if (snapshot.text.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    *result = std::move(snapshot); return S_OK;
}
HRESULT ExplorerApp::createAddressContextMenu(HMENU* result) const {
    if (!result) return E_POINTER;
    HMENU menu = nullptr;
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    if (length && length < std::size(system)) {
        const auto module = LoadLibraryExW((std::filesystem::path(system) / L"explorerframe.dll").c_str(),
            nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (module) {
            const auto resource = LoadMenuW(module, MAKEINTRESOURCEW(272));
            const auto popup = resource ? GetSubMenu(resource, 0) : nullptr;
            constexpr std::array ids{AddressContextCommand::Copy, AddressContextCommand::CopyText,
                AddressContextCommand::Edit, AddressContextCommand::DeleteHistory};
            bool matches = popup && GetMenuItemCount(popup) == static_cast<int>(ids.size());
            for (UINT index = 0; matches && index < ids.size(); ++index)
                matches = GetMenuItemID(popup, index) == static_cast<UINT>(ids[index]);
            if (matches && RemoveMenu(resource, 0, MF_BYPOSITION)) menu = popup;
            if (resource) DestroyMenu(resource);
            FreeLibrary(module);
        }
    }
    if (!menu) {
        menu = CreatePopupMenu();
        if (!menu) return HRESULT_FROM_WIN32(GetLastError());
        for (const auto& entry : std::array{
                 std::pair{AddressContextCommand::Copy, L"&Copy address"},
                 std::pair{AddressContextCommand::CopyText, L"C&opy address as text"},
                 std::pair{AddressContextCommand::Edit, L"&Edit address"},
                 std::pair{AddressContextCommand::DeleteHistory, L"&Delete history"}}) {
            if (!AppendMenuW(menu, MF_STRING, static_cast<UINT>(entry.first), entry.second)) {
                const auto error = HRESULT_FROM_WIN32(GetLastError()); DestroyMenu(menu); return error;
            }
        }
    }
    for (const auto command : {AddressContextCommand::Copy, AddressContextCommand::CopyText, AddressContextCommand::Edit})
        EnableMenuItem(menu, static_cast<UINT>(command), MF_BYCOMMAND | (currentPidl_ ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, static_cast<UINT>(AddressContextCommand::DeleteHistory), MF_BYCOMMAND |
        (!typedAddresses_.empty() || pendingTypedAddressTarget_ ? MF_ENABLED : MF_GRAYED));
    *result = menu; return S_OK;
}
HRESULT ExplorerApp::executeAddressContext(AddressContextCommand command, const AddressContextSnapshot& target) {
    if (closing_) return E_ABORT;
    if (command == AddressContextCommand::Edit) {
        if (!target.location || !currentPidl_ || !ILIsEqual(target.location.get(), currentPidl_.get()) || navigating_)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        editAddress(); SetWindowTextW(address_, target.text.c_str()); return S_OK;
    }
    // Copy and persistent history deletion must never alter shared state from
    // a headless fixture, even with an otherwise valid native menu snapshot.
    if (headless_) return E_ACCESSDENIED;
    if (command == AddressContextCommand::DeleteHistory) {
        const auto hr = saveAddressHistory(addressHistoryPath(), {});
        if (FAILED(hr)) return hr;
        typedAddresses_.clear(); pendingTypedAddress_.clear(); pendingTypedAddressTarget_.reset();
        addressHistoryStatus_ = hr; return hr;
    }
    if (!target.location || target.text.empty()) return E_INVALIDARG;
    if (command == AddressContextCommand::CopyText) return ShellOperations::copyText(window_, target.text);
    if (command != AddressContextCommand::Copy) return E_INVALIDARG;
    PCIDLIST_ABSOLUTE identity = target.location.get();
    ComPtr<IShellItemArray> items;
    const auto hr = SHCreateShellItemArrayFromIDLists(1, &identity, &items);
    return FAILED(hr) ? hr : ShellOperations::copyToClipboard(window_, items.Get(), false);
}
HRESULT ExplorerApp::showAddressContextMenu(POINT point) {
    if (headless_) return E_ACCESSDENIED;
    if (closing_ || navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    AddressContextSnapshot snapshot;
    auto hr = addressContextSnapshot(&snapshot);
    if (FAILED(hr)) return hr;
    HMENU menu = nullptr;
    hr = createAddressContextMenu(&menu);
    if (FAILED(hr)) return hr;
    UINT flags = 0;
    hr = popupUiFlags(window_, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, &flags);
    if (SUCCEEDED(hr) && point.x == -1 && point.y == -1) {
        RECT bounds{};
        if (!GetWindowRect(breadcrumbs_, &bounds)) hr = HRESULT_FROM_WIN32(GetLastError());
        UiPopupPlacement placement;
        if (SUCCEEDED(hr)) hr = popupUiPlacement(window_, bounds, flags, &placement);
        if (SUCCEEDED(hr)) { point = placement.anchor; flags = placement.flags; }
    }
    if (FAILED(hr)) { DestroyMenu(menu); return hr; }
    const auto command = TrackPopupMenuEx(menu, flags,
        point.x, point.y, window_, nullptr);
    DestroyMenu(menu);
    return command ? executeAddressContext(static_cast<AddressContextCommand>(command), snapshot) : S_FALSE;
}
void ExplorerApp::updateRibbonCollapseButton() {
    if(!ribbonCollapse_||!window_||!ribbon_.valid())return;
    bool minimized=false;
    if(FAILED(ribbon_.minimized(minimized)))return;
    UiString label, tip;
    if (FAILED(loadUiString(minimized ? UiText::ExpandRibbon : UiText::MinimiseRibbon, &label)) ||
        FAILED(loadUiString(minimized ? UiText::ExpandRibbonTooltip : UiText::MinimiseRibbonTooltip, &tip))) return;
    if(textOf(ribbonCollapse_)!=label.text)SetWindowTextW(ribbonCollapse_,label.text.c_str());
    if(tip.text!=ribbonCollapseTip_) {
        ribbonCollapseTip_=tip.text;
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
        if (FAILED(mapUiRect(nullptr, window_, tabs, &tabs))) return;
        SetWindowPos(ribbonCollapse_,HWND_TOP,std::max(0,static_cast<int>(tabs.right)-px(44)),tabs.top,
            px(22),px(24),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(ribbonCollapse_,nullptr,FALSE);
    }
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
HRESULT ExplorerApp::execute(UINT command) {
    if(closing_)return E_ABORT;
    if(command==Search) {
        if(!search_||!IsWindow(search_))return E_UNEXPECTED;
        ++searchInteractionRevision_;
        const auto submitted=liveSearchPolicy_.submit(textOf(search_),GetTickCount64());
        return FAILED(submitted)?submitted:processLiveSearch();
    }
    if(commandRefreshActive_) {deferCommandRefresh();return HRESULT_FROM_WIN32(ERROR_RETRY);}
    if(command==ExpandAncestors) {
        return applyNavigationOptions(true);
    }
    if(headless_&&command==OpenFileLocation)return openFileLocation();
    if(headless_&&command==NewFolder)return E_ACCESSDENIED;
    if(command==NewFolder&&librariesRoot_)return newLibrary();
    if (const auto binding = appCommandBinding(command); binding && binding->route != AppCommandRoute::Host)
        return executeRibbon(command);
    if (command == NewShortcut || command == NewText || command == LibraryLocations) return executeRibbon(command);
    if (command >= ViewFirst && command <= ViewLast) return setView(static_cast<ViewMode>(command - ViewFirst));
    ComPtr<IShellItemArray> items;
    switch (command) {
    case Back: return browseHistory(-1);
    case Forward: return browseHistory(1);
    case Up: return browser_->BrowseToIDList(nullptr, SBSP_PARENT);
    case Refresh: return view_ ? view_->Refresh() : E_UNEXPECTED;
    case Address: editAddress(); return S_OK;
    case BreadcrumbOverflow: return showBreadcrumbOverflow();
    case AddressList: popup(AddressList,breadcrumbs_);return headless_?E_ACCESSDENIED:S_OK;
    case FocusNext: return cycleFocus(false);
    case FocusPrevious: return cycleFocus(true);
    case Fullscreen: return toggleFullscreen();
    case FocusSearch: SetFocus(search_); SendMessageW(search_, EM_SETSEL, 0, -1); return S_OK;
    case CloseSearch:
        if(searchWindowOrigin_) {
            const auto origin=searchWindowOrigin_;setSearchText(L"");
            return browser_->BrowseToObject(origin.Get(),SBSP_ABSOLUTE);
        }
        if(liveSearchOrigin_) {
            ++searchInteractionRevision_;
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
    case NewLibrary: return newLibrary();
    case IncludeLibraryFolder: return includeLibraryFolder();
    case QuickAccessMenu: popup(command); return headless_ ? E_ACCESSDENIED : S_OK;
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
    case FileMenu: case HistoryMenu: case ViewMenu:
    case RecentSearches: case SearchKindMenu: case SearchDateMenu: case SearchSizeMenu:
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
    case Rename: return folderView_ ? folderView_->DoRename() : E_UNEXPECTED;
    case Properties: return showProperties(nullptr);
    case SharingProperties: return showProperties(L"Sharing");
    case Open: return nativeVerb(L"open");
    case Edit: return nativeVerb(L"edit");
    case Print: return nativeVerb(L"print");
    case Pin: return nativeVerb(L"pintohome", true);
    case SelectAll: case SelectNone: case Invert: {
        updateNamespace();
        CommandRefreshScope nativeCommand(*this);
        const auto action=command==SelectAll?SelectionAction::All:command==SelectNone?SelectionAction::None:SelectionAction::Invert;
        return changeShellSelection(folderView_.Get(),view_.Get(),action,&backgroundActions_,headless_);
    }
    case NavigationPane: case PreviewPane: case DetailsPane: case HiddenItems: break;
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
    case Checkboxes: {
        if (!folderView_) return E_UNEXPECTED;
        DWORD flags = 0;
        auto hr = folderView_->GetCurrentFolderFlags(&flags);
        if (FAILED(hr)) return hr;
        const bool checked = (flags & FWF_CHECKSELECT) == 0;
        hr = folderView_->SetCurrentFolderFlags(FWF_CHECKSELECT, checked ? FWF_CHECKSELECT : 0);
        if (FAILED(hr)) return hr;
        checkboxes_ = checked;
        namespaceDirty_ = selectionStateDirty_ = true;
        updateCommands();
        return hr;
    }
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
    default: return E_NOTIMPL;
    }
    return recreateBrowser(command);
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
        auto placementRead = mapUiRect(breadcrumbs_, nullptr, button, &button);
        UiPopupPlacement placement;
        if (SUCCEEDED(placementRead)) placementRead = popupUiPlacement(window_, button,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_TOPALIGN, &placement);
        if (FAILED(placementRead)) { DestroyMenu(menu); showError(placementRead, L"Recent locations"); return; }
        const auto selected=addresses.empty()?0:TrackPopupMenu(menu,placement.flags,
            placement.anchor.x,placement.anchor.y,0,window_,nullptr);
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
        if (SUCCEEDED(hr) && anchor) {
            RECT bounds{};
            if (!GetWindowRect(anchor, &bounds)) hr = HRESULT_FROM_WIN32(GetLastError());
            UiPopupPlacement placement;
            if (SUCCEEDED(hr)) hr = popupUiPlacement(window_, bounds, TPM_RETURNCMD | TPM_NONOTIFY, &placement);
            if (SUCCEEDED(hr)) point = placement.anchor;
        } else if (SUCCEEDED(hr) && !GetCursorPos(&point)) hr = HRESULT_FROM_WIN32(GetLastError());
        activeNamespaceMenu_=&actions;
        if (SUCCEEDED(hr)) hr = actions.invokeCommandStorePopup(native, false, point);
        activeNamespaceMenu_=nullptr;
        namespaceDirty_ = selectionStateDirty_ = true;
        if (SUCCEEDED(hr) && (command == LibraryDefault || command == LibraryOptimize || command == RibbonLibraryOptimizeMenu)) reloadLibrary();
        showError(hr, L"Command"); updateCommands(); return;
    }
    auto menu = CreatePopupMenu();
    auto add = [&](UINT id, const wchar_t* text, bool checked = false, bool disabled = false) {
        std::wstring label;
        for (const auto character : std::wstring_view(text)) { label += character; if (character == L'&') label += character; }
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (disabled ? MF_GRAYED : 0), id, label.c_str());
    };
    auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    std::vector<std::wstring> refinements;
    std::vector<Pidl> historyLocations;
    const auto queries = recentSearches_;
    if (command == QuickAccessMenu) {
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
        add(NewWindow, L"Open new window\tCtrl+N"); add(Terminal, L"Open Windows PowerShell here", false, !physicalDirectory_ || navigating_); separator();
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
    } else if (command == HistoryMenu) {
        historyLocations.reserve(history_.size());
        for (const auto& location : history_) {
            Pidl copy(ILCloneFull(location.get()));
            if (!copy) { DestroyMenu(menu); showError(E_OUTOFMEMORY, L"Open history menu"); return; }
            historyLocations.push_back(std::move(copy));
        }
        for (int i = static_cast<int>(history_.size()) - 1; i >= 0; --i) {
            auto name = pidlName(historyLocations[i].get(), SIGDN_NORMALDISPLAY);
            add(4000 + i, name.c_str(), i == historyIndex_);
        }
    } else if (command == RecentSearches) {
        for (size_t i = 0; i < queries.size(); ++i) add(27000 + static_cast<UINT>(i), queries[i].c_str());
    } else if (command == SearchKindMenu || command == SearchDateMenu || command == SearchSizeMenu) {
        const auto result = appendSearchRefinementMenu(command, menu, &refinements);
        if (FAILED(result)) { DestroyMenu(menu); showError(result, L"Read search refinements"); return; }
    }
    if (!anchor) anchor = command == FileMenu ? window_ : command == HistoryMenu ? nav_ : window_;
    POINT menuPoint{}; UINT menuFlags = 0;
    auto placementRead = popupUiFlags(window_, TPM_RETURNCMD | TPM_NONOTIFY | TPM_TOPALIGN, &menuFlags);
    if (SUCCEEDED(placementRead) && anchor) {
        RECT bounds{};
        if (!GetWindowRect(anchor, &bounds)) placementRead = HRESULT_FROM_WIN32(GetLastError());
        UiPopupPlacement placement;
        if (SUCCEEDED(placementRead)) placementRead = popupUiPlacement(window_, bounds, menuFlags, &placement);
        if (SUCCEEDED(placementRead)) { menuPoint = placement.anchor; menuFlags = placement.flags; }
    } else if (SUCCEEDED(placementRead) && !GetCursorPos(&menuPoint)) placementRead = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(placementRead)) { DestroyMenu(menu); showError(placementRead, L"Open menu"); return; }
    const auto selected = TrackPopupMenu(menu, menuFlags, menuPoint.x, menuPoint.y, 0, window_, nullptr);
    DestroyMenu(menu);
    bool toolbarChanged = false;
    const auto catalog = quickAccessCommands();
    const auto& commands = quickAccessModel_.commands();
    if (command == QuickAccessMenu && selected >= 30000 && selected - 30000 < catalog.size()) {
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
    } else if (command == HistoryMenu && selected >= 4000 && selected - 4000 < historyLocations.size()) {
        const auto index = selected - 4000;
        showError(browseHistoryLocation(historyLocations[index].get(), static_cast<int>(index)), L"Open history location");
    } else if (command == RecentSearches && selected >= 27000 && selected - 27000 < queries.size()) {
        setSearchText(queries[selected - 27000]);
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
    if (!headless_) {
        bool rightToLeft = false;
        windowUiDirection(window_, &rightToLeft);
        MessageBoxW(window_, lastError_.c_str(), L"Windows Explorer", MB_OK | MB_ICONERROR |
            (rightToLeft ? MB_RTLREADING | MB_RIGHT : 0));
    }
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
    // Native dialogs and auxiliary windows on this STA own their keyboard
    // messages. Only the Explorer host and its descendants can dispatch its
    // navigation/selection shortcuts or participate in its Tab cycle.
    if (!window_ || !message.hwnd || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
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
        if(focus&&focus!=window_&&!IsChild(window_,focus))return false;
        const auto command = shortcutCommand(static_cast<UINT>(key), control, shift, alt, inEdit);
        if (command) { showError(execute(*command), L"Command"); return true; }
        if(message.message==WM_KEYDOWN&&key==VK_RETURN&&!control&&!shift&&!alt&&
           (focus==nav_||focus==breadcrumbs_||focus==addressActions_)) {
            const auto index=SendMessageW(focus,TB_GETHOTITEM,0,0);TBBUTTON button{};
            if(index>=0&&SendMessageW(focus,TB_GETBUTTON,index,reinterpret_cast<LPARAM>(&button))&&
               (button.fsState&TBSTATE_ENABLED)&&!(button.fsState&TBSTATE_HIDDEN)&&!(button.fsStyle&BTNS_SEP)) {
                const auto toolbarCommand = static_cast<UINT>(button.idCommand);
                showError(focus == breadcrumbs_ && breadcrumbAncestor(toolbarCommand) ? browseBreadcrumb(toolbarCommand) : execute(toolbarCommand), L"Command");
                return true;
            }
        }
    }
    HWND nativeView = nullptr;
    if (view_ && SUCCEEDED(view_->GetWindow(&nativeView)) &&
        (message.hwnd == nativeView || IsChild(nativeView, message.hwnd)) &&
        view_->TranslateAccelerator(&message) == S_OK) return true;
    if (message.message == WM_KEYDOWN && message.wParam == VK_TAB &&
        !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        const auto focus=GetFocus();
        if(focus==nav_||focus==breadcrumbs_||focus==address_||focus==addressActions_||focus==search_)
            return cycleToolbarFocus((GetKeyState(VK_SHIFT)&0x8000)!=0)==S_OK;
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
            ++app->searchInteractionRevision_;
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
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wparam) == breadcrumbs_) {
            showError(showAddressContextMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}), L"Address menu");
            return 0;
        }
        break;
    case WM_CLIPBOARDUPDATE:
        cancelCommandStates();namespaceDirty_=true;
        if(!closing_&&!navigating_)updateCommands();
        return 0;
    case WM_SETCURSOR:
        if(LOWORD(lparam)==HTCLIENT) {
            POINT cursor{}; RECT searchBounds{};
            const bool mapped = GetCursorPos(&cursor) && GetWindowRect(search_, &searchBounds) &&
                SUCCEEDED(mapUiPoint(nullptr, window_, cursor, &cursor)) && SUCCEEDED(mapUiRect(nullptr, window_, searchBounds, &searchBounds));
            if(searchResizing_ || (mapped && cursor.x>=searchBounds.left-px(9)&&cursor.x<searchBounds.left&&cursor.y>=searchBounds.top&&cursor.y<searchBounds.bottom)) {
                SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT cursor{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        RECT searchBounds{};
        if (!GetWindowRect(search_, &searchBounds) || FAILED(mapUiRect(nullptr, window_, searchBounds, &searchBounds))) break;
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
        for (auto hwnd : {nav_, address_, breadcrumbs_, search_, addressActions_}) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        applyChrome(nav_, breadcrumbs_, address_, search_, addressActions_);
        applyWindowTheme(window_); applyRibbonTheme(ribbon_.framework());
        ribbon_.invalidate(); rebuildRibbon(); updateBreadcrumbs(); layout(); if (old) DeleteObject(old);
        return 0;
    }
    case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
        refreshProcessTheme(); applyWindowTheme(window_); applyRibbonTheme(ribbon_.framework());
        applyChrome(nav_, breadcrumbs_, address_, search_, addressActions_);
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
        if (reinterpret_cast<HWND>(lparam) == breadcrumbs_ && HIWORD(wparam) == BN_CLICKED &&
            breadcrumbAncestor(LOWORD(wparam))) {
            showError(browseBreadcrumb(LOWORD(wparam)), L"Open breadcrumb folder"); return 0;
        }
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
        if (!notification) return 0;
        if (notification->code == TBN_GETINFOTIPW &&
            (notification->hwndFrom == nav_ || notification->hwndFrom == addressActions_)) {
            const auto info = reinterpret_cast<NMTBGETINFOTIPW*>(lparam);
            UiText key = UiText::Count;
            if (notification->hwndFrom == nav_) {
                switch (info->iItem) {
                case Back: key = UiText::BackTooltip; break;
                case Forward: key = UiText::ForwardTooltip; break;
                case HistoryMenu: key = UiText::RecentLocations; break;
                case Up: key = UiText::UpTooltip; break;
                }
            } else if (info->iItem == Refresh) key = UiText::RefreshTooltip;
            UiString tip;
            const auto loaded = key == UiText::RefreshTooltip ? formatUiString(key, currentName_, &tip) :
                key != UiText::Count ? loadUiString(key, &tip) : E_INVALIDARG;
            if (SUCCEEDED(loaded) && info->pszText && info->cchTextMax > 0) {
                auto& stored = navigationTooltipText_[static_cast<UINT>(info->iItem)];
                if (stored != tip.text) stored = std::move(tip.text);
                auto count = std::min(stored.size(), static_cast<size_t>(info->cchTextMax - 1));
                if (count && count < stored.size() && stored[count - 1] >= 0xd800 && stored[count - 1] <= 0xdbff &&
                    stored[count] >= 0xdc00 && stored[count] <= 0xdfff) --count;
                std::copy_n(stored.data(), count, info->pszText);
                info->pszText[count] = L'\0';
            }
            return 0;
        }
        if (notification->code == NM_CUSTOMDRAW && (notification->hwndFrom == nav_ || notification->hwndFrom == addressActions_))
            return chromeToolbarCustomDraw(*reinterpret_cast<NMTBCUSTOMDRAW*>(lparam), dpi_);
        if(notification->code==NM_CUSTOMDRAW&&notification->hwndFrom==breadcrumbs_)
            return chromeBreadcrumbCustomDraw(*reinterpret_cast<NMTBCUSTOMDRAW*>(lparam),dpi_);
        if (notification->hwndFrom==breadcrumbs_ && notification->code==TBN_DROPDOWN) {
            const auto dropdown=reinterpret_cast<NMTOOLBARW*>(lparam);
            beginBreadcrumbMenu(static_cast<UINT>(dropdown->iItem)); return TBDDRET_NODEFAULT;
        }
        if (notification->hwndFrom == breadcrumbs_ && notification->code == NM_RCLICK) {
            const auto mouse = reinterpret_cast<NMMOUSE*>(lparam);
            auto point = mouse->pt;
            if (FAILED(mapUiPoint(breadcrumbs_, nullptr, point, &point))) return FALSE;
            showError(showAddressContextMenu(point), L"Address menu"); return TRUE;
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
            applyPendingSelection(); updateCommands();
        }
        return 0;
    case WM_APPCOMMAND:
        if (GET_APPCOMMAND_LPARAM(lparam) == APPCOMMAND_BROWSER_BACKWARD) { execute(Back); return TRUE; }
        if (GET_APPCOMMAND_LPARAM(lparam) == APPCOMMAND_BROWSER_FORWARD) { execute(Forward); return TRUE; }
        break;
    case DeferredUpdate:
        deferredUpdateQueued_=false;
        if(!closing_) {applyPendingSelection();updateCommands();}
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
    case NamespaceResult:
        if(closing_)return 0;
        if (FAILED(static_cast<HRESULT>(lparam))) showError(static_cast<HRESULT>(lparam), L"Offline files");
        namespaceDirty_ = true; updateCommands(); return 0;
    case DeferredView:
        if (!closing_&&folderView_ && !navigating_) {
            DWORD flags{};
            if(SUCCEEDED(folderView_->GetCurrentFolderFlags(&flags)))checkboxes_=(flags&FWF_CHECKSELECT)!=0;
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
            forgetRibbonTheme(ribbon_.framework());ribbon_.reset();destroyBrowser();
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
