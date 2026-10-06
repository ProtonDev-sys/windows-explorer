#include "explorer/ribbon.hpp"
#include "explorer/commands.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include "state_file.hpp"

#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uiautomation.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <thread>
#include <utility>

namespace explorer {
using Microsoft::WRL::ComPtr;
namespace {
struct Variant {
    PROPVARIANT value{};
    ~Variant() { PropVariantClear(&value); }
};
// Shared lifetime makes the exit bump safe even if a callback resets the host.
// Every host QAT/state writer and genuine Execute enters and exits a scope.
struct RibbonRevision { std::uint64_t value=0; bool windowDestroyed=false,retired=false; };
struct RibbonMutation {
    std::shared_ptr<RibbonRevision> revision;
    explicit RibbonMutation(std::shared_ptr<RibbonRevision> value):revision(std::move(value)){++revision->value;}
    ~RibbonMutation(){++revision->value;}
};

struct TabSelection {
    HWND window = nullptr;
    HWND ribbonWindow = nullptr;
    UINT nativeCommand = 0;
    UINT commandType = UI_COMMANDTYPE_UNKNOWN;
    std::wstring name;
    std::wstring desktop;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HRESULT> result{E_PENDING};
    std::atomic<bool> cancelled{false};
    bool searchDateMenu = false;
    std::vector<std::wstring> expectedRows;
    UINT matchedRows = 0;
    NativePopupCapture popup;
    ~TabSelection() { if(done)CloseHandle(done); }
};

HRESULT expandNativeSearchDateMenu(IUIAutomation* automation, TabSelection& request) {
    request.popup.stage=1;
    request.popup.ribbonWindow=reinterpret_cast<UINT_PTR>(request.ribbonWindow);
    request.popup.nativeCommand=request.nativeCommand;request.popup.commandType=request.commandType;
    DWORD ribbonProcess=0;
    const auto ribbonThread=GetWindowThreadProcessId(request.ribbonWindow,&ribbonProcess);
    if(!request.ribbonWindow||!IsWindow(request.ribbonWindow)||ribbonProcess!=GetCurrentProcessId()||
        ribbonThread!=GetWindowThreadProcessId(request.window,nullptr)||!IsChild(request.window,request.ribbonWindow)||
        request.commandType!=UI_COMMANDTYPE_COLLECTION||!request.nativeCommand)return E_ACCESSDENIED;
    if(!GetWindowRect(request.ribbonWindow,&request.popup.ribbonBounds))return HRESULT_FROM_WIN32(GetLastError());
    ComPtr<IUIAutomationElement> ribbonRoot;
    auto hr=automation->ElementFromHandle(request.ribbonWindow,&ribbonRoot);if(FAILED(hr))return hr;
    ComPtr<IUIAutomationElement> root;
    hr=automation->ElementFromHandle(request.window,&root);if(FAILED(hr))return hr;
    VARIANT caption{};caption.vt=VT_BSTR;caption.bstrVal=SysAllocString(request.name.c_str());
    if(!caption.bstrVal)return E_OUTOFMEMORY;
    ComPtr<IUIAutomationCondition> named;
    hr=automation->CreatePropertyCondition(UIA_NamePropertyId,caption,&named);VariantClear(&caption);
    if(FAILED(hr))return hr;
    ComPtr<IUIAutomationElementArray> candidates;
    request.popup.stage=2;
    hr=root->FindAll(TreeScope_Descendants,named.Get(),&candidates);if(FAILED(hr))return hr;
    int count=0;hr=candidates?candidates->get_Length(&count):E_UNEXPECTED;if(FAILED(hr))return hr;
    request.popup.candidateCount=static_cast<UINT>(count);
    if(count<0||count>4096)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    request.popup.stage=3;
    ComPtr<IUIAutomationTreeWalker> walker;
    const auto walkerRead=automation->get_ControlViewWalker(&walker);
    ComPtr<IUIAutomationTreeWalker> rawWalker;
    const auto rawWalkerRead=automation->get_RawViewWalker(&rawWalker);
    ComPtr<IUIAutomationExpandCollapsePattern> expand;
    for(int index=0;index<count;++index) {
        if(request.cancelled)return E_ABORT;
        NativePopupCapture::ParentReadback observed;
        ComPtr<IUIAutomationElement> candidate;BOOL enabled=FALSE,offscreen=TRUE;
        observed.elementRead=candidates->GetElement(index,&candidate);
        ComPtr<IUIAutomationExpandCollapsePattern> pattern;
        ExpandCollapseState state=ExpandCollapseState_LeafNode;
        if(SUCCEEDED(observed.elementRead)&&candidate) {
            observed.enabledRead=candidate->get_CurrentIsEnabled(&enabled);observed.enabled=enabled!=FALSE;
            observed.offscreenRead=candidate->get_CurrentIsOffscreen(&offscreen);observed.offscreen=offscreen!=FALSE;
            observed.typeRead=candidate->get_CurrentControlType(&observed.type);
            observed.boundsRead=candidate->get_CurrentBoundingRectangle(&observed.bounds);
            BSTR automationId=nullptr;
            observed.automationIdRead=candidate->get_CurrentAutomationId(&automationId);
            if(SUCCEEDED(observed.automationIdRead)&&automationId)observed.automationId=automationId;
            SysFreeString(automationId);
            observed.patternRead=candidate->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,IID_PPV_ARGS(&pattern));
            if(SUCCEEDED(observed.patternRead)&&pattern)observed.stateRead=pattern->get_CurrentExpandCollapseState(&state);
            observed.expandState=static_cast<int>(state);
            observed.parentRead=walkerRead;
            ComPtr<IUIAutomationElement> parent;
            if(SUCCEEDED(observed.parentRead))observed.parentRead=walker?walker->GetParentElement(candidate.Get(),&parent):E_UNEXPECTED;
            if(SUCCEEDED(observed.parentRead))observed.parentRead=parent?parent->get_CurrentControlType(&observed.parentType):E_UNEXPECTED;
            if(SUCCEEDED(observed.parentRead))observed.parentRead=parent->get_CurrentBoundingRectangle(&observed.parentBounds);
            BSTR parentName=nullptr;
            if(SUCCEEDED(observed.parentRead))observed.parentRead=parent->get_CurrentName(&parentName);
            observed.parentSameName=SUCCEEDED(observed.parentRead)&&parentName&&request.name==parentName;
            SysFreeString(parentName);
            const auto contains=[](const RECT& outer,const RECT& inner) {
                return inner.right>inner.left&&inner.bottom>inner.top&&inner.left>=outer.left&&
                    inner.top>=outer.top&&inner.right<=outer.right&&inner.bottom<=outer.bottom;
            };
            observed.ribbonBoundsContain=SUCCEEDED(observed.boundsRead)&&contains(request.popup.ribbonBounds,observed.bounds);
            observed.toolbarBoundsContain=SUCCEEDED(observed.parentRead)&&contains(observed.parentBounds,observed.bounds)&&
                contains(request.popup.ribbonBounds,observed.parentBounds);
            observed.ribbonAncestorRead=rawWalkerRead;
            ComPtr<IUIAutomationElement> ancestor=candidate;
            for(UINT depth=0;SUCCEEDED(observed.ribbonAncestorRead)&&ancestor&&depth<128;++depth) {
                if(request.cancelled)return E_ABORT;
                BOOL same=FALSE;
                observed.ribbonAncestorRead=automation->CompareElements(ancestor.Get(),ribbonRoot.Get(),&same);
                if(FAILED(observed.ribbonAncestorRead))break;
                if(same){observed.ribbonAncestor=true;break;}
                ComPtr<IUIAutomationElement> next;
                observed.ribbonAncestorRead=rawWalker?rawWalker->GetParentElement(ancestor.Get(),&next):E_UNEXPECTED;
                ancestor=std::move(next);
            }
        }
        // The installed Date collection's whole gallery is a SplitButton under
        // a Ribbon toolbar. Identically named folder headers/row edits and
        // split-button action/arrow children are outside this exact domain.
        observed.accepted=SUCCEEDED(observed.enabledRead)&&observed.enabled&&SUCCEEDED(observed.offscreenRead)&&!observed.offscreen&&
            SUCCEEDED(observed.typeRead)&&observed.type==UIA_SplitButtonControlTypeId&&
            SUCCEEDED(observed.parentRead)&&observed.parentType==UIA_ToolBarControlTypeId&&!observed.parentSameName&&
            observed.ribbonBoundsContain&&observed.toolbarBoundsContain&&SUCCEEDED(observed.ribbonAncestorRead)&&
            observed.ribbonAncestor&&SUCCEEDED(observed.patternRead)&&pattern&&
            SUCCEEDED(observed.stateRead)&&state!=ExpandCollapseState_LeafNode;
        request.popup.parents.push_back(observed);
        if(observed.accepted) {
            // Reject ambiguous parents before any action. Filter leaves never
            // receive Invoke, Select, focus or any other action.
            if(expand)return E_UNEXPECTED;
            expand=std::move(pattern);
        }
    }
    request.popup.stage=4;
    if(!expand)return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if(request.cancelled)return E_ABORT;
    request.popup.stage=5;
    hr=expand->Expand();if(FAILED(hr))return hr;
    ComPtr<IUIAutomationCondition> everything;
    hr=automation->CreateTrueCondition(&everything);if(FAILED(hr))return hr;
    struct Popups {HWND host;std::vector<HWND> windows;} popups{request.window,{}};
    const auto collect=[](HWND popup,LPARAM value)->BOOL {
        auto& found=*reinterpret_cast<Popups*>(value);DWORD process=0;GetWindowThreadProcessId(popup,&process);
        if(process==GetCurrentProcessId()&&popup!=found.host&&IsWindowVisible(popup)&&
            GetAncestor(popup,GA_ROOTOWNER)==found.host)found.windows.push_back(popup);
        return TRUE;
    };
    const auto deadline=GetTickCount64()+2000;
    request.popup.stage=6;
    do {
        if(request.cancelled)return E_ABORT;
        ExpandCollapseState state=ExpandCollapseState_LeafNode;
        hr=expand->get_CurrentExpandCollapseState(&state);if(FAILED(hr))return hr;
        popups.windows.clear();
        EnumThreadWindows(GetWindowThreadProcessId(request.window,nullptr),collect,reinterpret_cast<LPARAM>(&popups));
        request.matchedRows=0;
        NativePopupCapture completePopup;
        for(const auto popup:popups.windows) {
            RECT popupBounds{};
            if(!GetWindowRect(popup,&popupBounds))continue;
            std::vector<bool> matched(request.expectedRows.size(),false);
            std::vector<RECT> matchedBounds(request.expectedRows.size());
            ComPtr<IUIAutomationElement> popupRoot;
            if(FAILED(automation->ElementFromHandle(popup,&popupRoot))||!popupRoot)continue;
            ComPtr<IUIAutomationElementArray> rows;
            if(FAILED(popupRoot->FindAll(TreeScope_Descendants,everything.Get(),&rows))||!rows)continue;
            int rowCount=0;hr=rows->get_Length(&rowCount);if(FAILED(hr))return hr;
            for(int index=0;index<rowCount;++index) {
                if(request.cancelled)return E_ABORT;
                ComPtr<IUIAutomationElement> row;BOOL offscreen=TRUE;RECT bounds{};CONTROLTYPEID type=0;
                if(FAILED(rows->GetElement(index,&row))||!row||FAILED(row->get_CurrentControlType(&type))||
                    (type!=UIA_MenuItemControlTypeId&&type!=UIA_ListItemControlTypeId&&type!=UIA_ButtonControlTypeId)||
                    FAILED(row->get_CurrentIsOffscreen(&offscreen))||offscreen||
                    FAILED(row->get_CurrentBoundingRectangle(&bounds))||bounds.right<=bounds.left||bounds.bottom<=bounds.top||
                    bounds.left<popupBounds.left||bounds.top<popupBounds.top||
                    bounds.right>popupBounds.right||bounds.bottom>popupBounds.bottom)continue;
                BSTR name=nullptr;
                if(SUCCEEDED(row->get_CurrentName(&name))&&name)
                    for(size_t wanted=0;wanted<matched.size();++wanted)if(request.expectedRows[wanted]==name) {
                        matched[wanted]=true;matchedBounds[wanted]=bounds;
                    }
                SysFreeString(name);
            }
            const auto matchedCount=static_cast<UINT>(std::count(matched.begin(),matched.end(),true));
            request.matchedRows=std::max(request.matchedRows,matchedCount);
            if(matchedCount==request.expectedRows.size()) {
                if(completePopup.window)return E_UNEXPECTED;
                completePopup.window=popup;completePopup.bounds=popupBounds;
                completePopup.rowBounds=std::move(matchedBounds);
            }
        }
        if(state==ExpandCollapseState_Expanded&&completePopup.window) {
            completePopup.stage=7;completePopup.candidateCount=request.popup.candidateCount;
            completePopup.parents=std::move(request.popup.parents);
            completePopup.ribbonWindow=request.popup.ribbonWindow;completePopup.ribbonBounds=request.popup.ribbonBounds;
            completePopup.nativeCommand=request.popup.nativeCommand;completePopup.commandType=request.popup.commandType;
            request.popup=std::move(completePopup);return S_OK;
        }
        Sleep(10); // The creator STA continues dispatching while rows materialize.
    }while(GetTickCount64()<deadline);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

HRESULT selectNativeTab(IUIAutomation* automation, const TabSelection& request) {
    if(request.cancelled)return E_ABORT;
    ComPtr<IUIAutomationElement> root;
    auto hr=automation->ElementFromHandle(request.window,&root);if(FAILED(hr))return hr;
    VARIANT type{};type.vt=VT_I4;type.lVal=UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> typeCondition;
    hr=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&typeCondition);if(FAILED(hr))return hr;
    VARIANT caption{};caption.vt=VT_BSTR;caption.bstrVal=SysAllocString(request.name.c_str());if(!caption.bstrVal)return E_OUTOFMEMORY;
    ComPtr<IUIAutomationCondition> nameCondition;
    hr=automation->CreatePropertyCondition(UIA_NamePropertyId,caption,&nameCondition);VariantClear(&caption);if(FAILED(hr))return hr;
    ComPtr<IUIAutomationCondition> condition;
    hr=automation->CreateAndCondition(typeCondition.Get(),nameCondition.Get(),&condition);if(FAILED(hr))return hr;
    if(request.cancelled)return E_ABORT;
    ComPtr<IUIAutomationElement> element;
    const auto deadline=GetTickCount64()+2000;
    do {
        if(request.cancelled)return E_ABORT;
        hr=root->FindFirst(TreeScope_Descendants,condition.Get(),&element);if(FAILED(hr))return hr;
        if(element)break;
        // SetModes publishes its accessible tab tree asynchronously. Observe
        // the identical owned root and exact tab name/type while its STA pumps.
        Sleep(20);
    }while(GetTickCount64()<deadline);
    if(!element)return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if(request.cancelled)return E_ABORT;
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    hr=element->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&selection));if(FAILED(hr))return hr;
    if(!selection)return E_NOINTERFACE;
    BOOL selected=FALSE;hr=selection->get_CurrentIsSelected(&selected);
    if(FAILED(hr)||selected)return hr;
    if(request.cancelled)return E_ABORT;
    hr=selection->Select();if(FAILED(hr))return hr;
    do {
        if(request.cancelled)return E_ABORT;
        hr=selection->get_CurrentIsSelected(&selected);if(FAILED(hr)||selected)return hr;
        Sleep(20);
    }while(GetTickCount64()<deadline);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

HRESULT selectTabOnMta(HWND window,const wchar_t* name,
                       std::span<const std::wstring> expectedRows={},UINT* matchedRows=nullptr,
                       NativePopupCapture* popup=nullptr,HWND ribbonWindow=nullptr,
                       UINT nativeCommand=0,UINT commandType=UI_COMMANDTYPE_UNKNOWN) {
    // Microsoft requires own-UI automation to run on a separate windowless MTA.
    // Only HWND/name cross apartments; no Ribbon COM object leaves its STA.
    auto request=std::make_shared<TabSelection>();
    if(!request->done)return HRESULT_FROM_WIN32(GetLastError());
    request->window=window;request->name=name;
    request->ribbonWindow=ribbonWindow;request->nativeCommand=nativeCommand;request->commandType=commandType;
    request->searchDateMenu=!expectedRows.empty();
    request->expectedRows.assign(expectedRows.begin(),expectedRows.end());
    wchar_t desktopName[256]{};DWORD bytes=0;
    if(!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktopName,sizeof(desktopName),&bytes))
        return HRESULT_FROM_WIN32(GetLastError());
    request->desktop=desktopName;
    std::thread worker([request] {
        const auto original=GetThreadDesktop(GetCurrentThreadId());
        const auto desktop=OpenDesktopW(request->desktop.c_str(),0,FALSE,DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_HOOKCONTROL);
        HRESULT result=desktop?S_OK:HRESULT_FROM_WIN32(GetLastError());
        if(SUCCEEDED(result)&&!SetThreadDesktop(desktop))result=HRESULT_FROM_WIN32(GetLastError());
        const auto initialized=SUCCEEDED(result)?CoInitializeEx(nullptr,COINIT_MULTITHREADED):result;
        if(SUCCEEDED(initialized)) {
            CoEnableCallCancellation(nullptr);
            {
                ComPtr<IUIAutomation2> automation;
                result=CoCreateInstance(CLSID_CUIAutomation8,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
                if(SUCCEEDED(result)) {
                    automation->put_ConnectionTimeout(1000);automation->put_TransactionTimeout(2000);automation->put_AutoSetFocus(FALSE);
                    result=request->searchDateMenu?expandNativeSearchDateMenu(automation.Get(),*request):
                        selectNativeTab(automation.Get(),*request);
                }
            }
            CoDisableCallCancellation(nullptr);CoUninitialize();
        } else result=initialized;
        if(desktop) { SetThreadDesktop(original);CloseDesktop(desktop); }
        request->result=result;SetEvent(request->done);
    });
    const auto deadline=GetTickCount64()+5000;
    bool complete=false;
    while(GetTickCount64()<deadline) {
        const auto waited=MsgWaitForMultipleObjectsEx(1,&request->done,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(waited==WAIT_OBJECT_0) {complete=true;break;}
        MSG message{};
        unsigned dispatched=0;
        while(dispatched++<16&&GetTickCount64()<deadline&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT) {PostQuitMessage(static_cast<int>(message.wParam));request->cancelled=true;break;}
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(request->cancelled)break;
    }
    if(!complete) {
        request->cancelled=true;
        CoCancelCall(GetThreadId(static_cast<HANDLE>(worker.native_handle())),0);
        const auto cancellationDeadline=GetTickCount64()+500;
        while(GetTickCount64()<cancellationDeadline) {
            if(MsgWaitForMultipleObjectsEx(1,&request->done,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_OBJECT_0) {complete=true;break;}
            MSG message{};unsigned dispatched=0;
            while(dispatched++<16&&GetTickCount64()<cancellationDeadline&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_QUIT){PostQuitMessage(static_cast<int>(message.wParam));break;}
                TranslateMessage(&message);DispatchMessageW(&message);
            }
        }
    }
    if(complete)worker.join();else worker.detach();
    if(complete&&matchedRows)*matchedRows=request->matchedRows;
    if(complete&&popup)*popup=std::move(request->popup);
    return complete?request->result.load():HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
struct RegistryCommand { UINT command; const wchar_t* key; };
struct CompiledLabel { UINT command; const wchar_t* label; };
bool englishPresentationLanguage() {
    // Read the same ordered thread/process/user/system fallback list used by
    // the resource loader. Regional date/number settings are not UI language.
    constexpr DWORD flags=MUI_LANGUAGE_NAME|MUI_UI_FALLBACK;
    ULONG count=0,characters=0;
    if(GetThreadPreferredUILanguages(flags,&count,nullptr,&characters)&&characters>1) {
        std::vector<wchar_t> languages(characters,L'\0');
        if(GetThreadPreferredUILanguages(flags,&count,languages.data(),&characters)&&count&&languages.front()) {
            wchar_t language[16]{};
            if(GetLocaleInfoEx(languages.data(),LOCALE_SISO639LANGNAME,language,static_cast<int>(std::size(language))))
                return wcscmp(language,L"en")==0;
        }
    }
    return PRIMARYLANGID(GetThreadUILanguage())==LANG_ENGLISH;
}
constexpr CompiledLabel compiledLabels[]{
#include "../resources/ribbon_labels.inc"
};
constexpr RegistryCommand registryCommands[]{
    {RibbonRemoveMediaServer,L"Windows.RemoveMediaServer"}, {RibbonOpenSearchViewSite,L"Windows.OpenSearchViewSite"},
    {RibbonGroupSortAscending,L"Windows.SortGroupsAscending"}, {RibbonGroupSortDescending,L"Windows.SortGroupsDescending"},
    {RibbonLibraryOptimizeMenu,L"Windows.LibraryOptimizeLibraryFor"},
    {RibbonCloudBackup,L"Windows.CloudBackup"}, {RibbonBackup,L"Windows.Backup"},
    {RibbonAddNetworkDevice,L"Windows.AddDevice"}, {RibbonDeviceWebpage,L"Windows.NetworkViewDeviceWebpage"},
    {RibbonConnectRemotePrinter,L"Windows.ViewRemotePrinters"}, {RibbonSearchActiveDirectory,L"Windows.SearchActiveDirectory"},
    {RibbonNetworkSharingCenter,L"Windows.NetworkAndSharing"}, {RibbonShortcutOpenLocation,L"Windows.Shortcut.opencontaining"},
    {RibbonLibraryPublicSaveLocation,L"Windows.LibraryPublicSaveLocation"}, {RibbonRemoveProperties,L"Windows.removeproperties"},
    {RibbonDeleteConfirmation,L"Windows.ToggleRecycleConfirmations"},
    {RibbonRunAsAnotherUser,L"Windows.runasuser"},
    {RibbonClearSearchHistory,L"Windows.SearchClearMru"},
    {RibbonOpenWith,L"Windows.OpenWith"},
    {RibbonPinToTaskbar,L"Windows.taskbarpin"},
    {RibbonExtractToGallery,L"Windows.CompressedFile.ExtractTo"},
    {RibbonAutoPlay,L"Windows.Autoplay"}, {RibbonFinishBurning,L"Windows.FinishBurn"}, {RibbonEraseDisc,L"Windows.EraseDisc"},
    {RibbonSearchThisPC,L"Windows.SearchSendToComputer"}, {ThisPC,L"Windows.SearchSendToComputer"},
    {RibbonBitLocker,L"Windows.BitLocker"}, {RibbonMountDiscImage,L"Windows.mount"},
    {RibbonBurnDiscImage,L"Windows.DiscImage.burn"}, {RibbonCastToDevice,L"Windows.PlayTo"},
    {RibbonWorkOffline,L"Windows.CscWorkOfflineOnline"}, {RibbonSyncOffline,L"Windows.CscSync"},
    {Pin,L"Windows.PinToHome"}, {Copy,L"Windows.copy"}, {Cut,L"Windows.cut"},
    {Paste,L"Windows.paste"}, {CopyPath,L"Windows.copyaspath"}, {PasteShortcut,L"Windows.pastelink"},
    {MoveTo,L"Windows.MoveToMenu"}, {CopyTo,L"Windows.CopyToMenu"}, {Delete,L"Windows.recycle"},
    {RibbonMoveMenu,L"Windows.MoveToMenu"}, {RibbonCopyMenu,L"Windows.CopyToMenu"},
    {RibbonDeleteMenu,L"Windows.RibbonDelete"}, {PermanentDelete,L"Windows.PermanentDelete"},
    {Rename,L"Windows.rename"}, {NewFolder,L"Windows.newfolder"}, {RibbonNewMenu,L"Windows.newitem"}, {NewItems,L"Windows.newitem"},
    {RibbonEasyAccessMenu,L"Windows.organize"}, {Properties,L"Windows.properties"},
    {RibbonPropertiesMenu,L"Windows.properties"},
    {RibbonOpenMenu,L"Windows.open"}, {Open,L"Windows.open"}, {Edit,L"Windows.edit"},
    {FileHistory,L"Windows.HistoryVaultRestore"}, {SelectAll,L"Windows.selectall"},
    {SelectNone,L"Windows.selectnone"}, {Invert,L"Windows.invertselection"},
    {Sharing,L"Windows.ModernShare"}, {RibbonEmail,L"Windows.email"}, {Zip,L"Windows.zip"},
    {RibbonBurnDisc,L"Windows.burn"}, {Print,L"Windows.print"}, {RibbonFax,L"Windows.fax"},
    {RibbonSpecificPeople,L"Windows.ShareSpecificUsers"}, {RibbonStopSharing,L"Windows.SharePrivate"},
    {Security,L"Windows.RibbonPermissionsDialog"}, {RibbonNavigationMenu,L"Windows.navpane"},
    {NavigationPane,L"Windows.navpane"}, {PreviewPane,L"Windows.readingpane"},
    {DetailsPane,L"Windows.previewpane"}, {SortMenu,L"Windows.SortByColumn"},
    {GroupMenu,L"Windows.GroupByColumn"}, {ColumnsMenu,L"Windows.AddColumns"},
    {SizeColumns,L"Windows.SizeAllColumns"}, {Checkboxes,L"Windows.SelectionCheckboxes"},
    {Extensions,L"Windows.ShowFileExtensions"}, {HiddenItems,L"Windows.ShowHiddenFiles"},
    {HideSelected,L"Windows.HideSelected"}, {RibbonOptionsMenu,L"Windows.folderoptions"},
    {RibbonFolderOptions,L"Windows.folderoptions"}, {FolderOptions,L"Windows.folderoptions"}, {NewWindow,L"Windows.location.opennewwindow"},
    {RibbonNewWindowMenu,L"Windows.location.opennewwindow"}, {RibbonNewProcess,L"Windows.location.opennewprocess"},
    {Terminal,L"Windows.location.Powershell"}, {RibbonPowerShellMenu,L"Windows.location.Powershell"},
    {RibbonPowerShellAdmin,L"Windows.location.PowershellAsAdmin"}, {Close,L"Windows.closewindow"},
    {RibbonHelp,L"Windows.help"}, {RibbonHelpButton,L"Windows.help"}, {RibbonAbout,L"Windows.aboutWindows"},
    {Undo,L"Windows.undo"}, {Redo,L"Windows.redo"}, {MapDrive,L"Windows.MapNetworkDrive"},
    {RibbonMapMenu,L"Windows.MapNetworkDrive"}, {DisconnectDrive,L"Windows.DisconnectNetworkDrive"},
    {RibbonAddNetworkLocation,L"Windows.AddNetworkLocation"}, {RibbonAccessMedia,L"Windows.AddMediaServer"},
    {RibbonSystemProperties,L"Windows.SystemProperties"}, {RibbonUninstallProgram,L"Windows.AddRemovePrograms"},
    {RibbonManageComputer,L"Windows.Computer.Manage"}, {RibbonConnectRemote,L"Windows.remotedesktop"},
    {RibbonRotateLeft,L"Windows.rotate270"}, {RibbonRotateRight,L"Windows.rotate90"},
    {RibbonSlideShow,L"Windows.slideshow"}, {RibbonSetBackground,L"Windows.setdesktopwallpaper"},
    {RibbonOptimizeDrive,L"Windows.Defragment"}, {RibbonDiskCleanup,L"Windows.CleanUp"},
    {RibbonFormatDrive,L"Windows.DiskFormat"}, {RibbonEjectDrive,L"Windows.Eject"},
    {Extract,L"Windows.CompressedFile.extract"}, {RibbonRestoreAll,L"Windows.RecycleBin.RestoreAll"},
    {RibbonRestoreSelected,L"Windows.RecycleBin.RestoreItems"}, {RibbonEmptyRecycleBin,L"Windows.RecycleBin.Empty"},
    {RibbonRecycleProperties,L"Windows.RecycleBin.properties"}, {RibbonRunAsAdministrator,L"Windows.runas"},
    {RibbonTroubleshootCompatibility,L"Windows.Troubleshoot"}, {RibbonPinToStart,L"Windows.pintostartscreen"},
    {RibbonPlay,L"Windows.play"}, {RibbonPlayAll,L"Windows.playall"}, {RibbonAddToPlaylist,L"Windows.Enqueue"},
    {LibraryLocations,L"Windows.LibraryManageLibrary"}, {IncludeLibraryFolder,L"Windows.LibraryIncludeInLibrary"},
    {LibraryDefault,L"Windows.LibraryDefaultSaveLocation"}, {LibraryOptimize,L"Windows.LibraryOptimizeLibraryFor"},
    {RibbonResetLibrary,L"Windows.LibraryRestoreDefaults"}, {SearchKindMenu,L"Windows.SearchFilterKind"},
    {RibbonLibraryChangeIcon,L"Windows.LibraryChangeIcon"}, {RibbonLibraryShowInNavigation,L"Windows.LibraryShowInNavPane"},
    {SearchDateMenu,L"Windows.SearchFilterDate"}, {SearchSizeMenu,L"Windows.SearchFilterSize"},
    {RibbonSearchOtherProperties,L"Windows.SearchFilterMoreProperties"}, {RecentSearches,L"Windows.SearchMru"},
    {SaveSearch,L"Windows.SearchSave"}, {OpenFileLocation,L"Windows.SearchOpenLocation"},
    {CloseSearch,L"Windows.SearchCloseTab"}, {SearchSubfolders,L"Windows.SearchOptionDeep"},
    {SearchCurrent,L"Windows.SearchOptionShallow"}, {RibbonSearchContents,L"Windows.SearchOptionContents"},
    {RibbonSearchSystemFiles,L"Windows.SearchOptionSystem"}, {RibbonSearchZipFiles,L"Windows.SearchOptionCompressed"},
    {RibbonChangeIndexedLocations,L"Windows.ChangeIndexedLocations"},
    {RibbonIncludeInLibrary,L"Windows.includeinlibrary"}, {RibbonAddToFavorites,L"Windows.AddToFavorites"},
    {RibbonMapAsDrive,L"Windows.MapNetworkDrive"}, {RibbonAlwaysAvailableOffline,L"Windows.CscWorkOfflineOnline"},
    {RibbonExpandToCurrent,L"Windows.NavPaneExpandToCurrentFolder"},
    {RibbonShowAllFolders,L"Windows.NavPaneShowAllFolders"}, {RibbonShowLibraries,L"Windows.NavPaneShowLibraries"},
    {SortAscending,L"Windows.SortAscending"}, {SortDescending,L"Windows.SortDescending"},
};
constexpr std::pair<UINT,UINT> groupIcons[]{
#include "../resources/ribbon_group_icons.inc"
};
constexpr std::array viewLabels{L"Extra large icons",L"Large icons",L"Medium icons",L"Small icons",L"List",L"Details",L"Tiles",L"Content"};
struct StockAlias { UINT native, application; };
constexpr StockAlias stockAliases[]{
#include "ribbon_stock.inc"
};
UINT stockNativeId(UINT application) noexcept {
    if(!application)return 0;
    if(application>=0x9000)return application<=0xfffe?application:0;
    switch(application) {
    case RibbonOpenMenu:return 0x3062;
    case RibbonFolderOptions:return 0x4061;
    case NewItems:return 0x4030;
    case ColumnsMenu:return 0x40c3;
    case LibraryOptimize:return 0x4962;
    default:break;
    }
    for(const auto& alias:stockAliases)if(alias.application==application)return alias.native;
    return 0;
}
UINT stockApplicationId(UINT native) noexcept {
    for(const auto& alias:stockAliases)if(alias.native==native)return alias.application;
    return 0;
}
HMODULE compatibleStockRibbon() noexcept {
    using VersionFunction=LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll=GetModuleHandleW(L"ntdll.dll");
    const auto versionFunction=ntdll?reinterpret_cast<VersionFunction>(GetProcAddress(ntdll,"RtlGetVersion")):nullptr;
    OSVERSIONINFOW version{sizeof(version)};
    if(!versionFunction||versionFunction(&version)<0||version.dwMajorVersion!=10||version.dwBuildNumber!=19045)return nullptr;
    const auto module=LoadLibraryExW(L"ExplorerFrame.dll",nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!module)return nullptr;
    const auto resource=FindResourceW(module,L"EXPLORER_RIBBON",L"UIFILE");
    const auto size=resource?SizeofResource(module,resource):0;
    const auto loaded=resource?LoadResource(module,resource):nullptr;
    const auto bytes=loaded?static_cast<const BYTE*>(LockResource(loaded)):nullptr;
    if(!bytes||size!=39972||memcmp(bytes+9,"SCBin$",6)) {FreeLibrary(module);return nullptr;}
    // The fixed alias table applies only to this verified installed symbol set.
    for(const auto symbol:{L"cmdTabHome",L"cmdQAT",L"cmdZipExtractTo"}) {
        const auto count=wcslen(symbol)*sizeof(wchar_t);
        bool present=false;
        for(DWORD offset=0;offset+count<=size;++offset)
            if(!memcmp(bytes+offset,symbol,count)) {present=true;break;}
        if(!present) {FreeLibrary(module);return nullptr;}
    }
    return module;
}
struct StockTranslation {
    std::map<UINT,UINT> overrides;
    UINT nativeId(UINT command) const noexcept {
        if(const auto found=overrides.find(command);found!=overrides.end())return found->second;
        return stockNativeId(command);
    }
};
class StockFramework final : public IUIFramework, public IPropertyStore {
public:
    StockFramework(IUIFramework* native,std::shared_ptr<StockTranslation> translation,std::shared_ptr<RibbonRevision> revision):
        native_(native),translation_(std::move(translation)),revision_(std::move(revision)) {
        native_->QueryInterface(IID_PPV_ARGS(&properties_));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(iid==IID_IUnknown||iid==__uuidof(IUIFramework))*output=static_cast<IUIFramework*>(this);
        else if(iid==__uuidof(IPropertyStore)&&properties_)*output=static_cast<IPropertyStore*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto value=--references_;if(!value)delete this;return value;}
    HRESULT STDMETHODCALLTYPE Initialize(HWND window,IUIApplication* application) override{return native_->Initialize(window,application);}
    HRESULT STDMETHODCALLTYPE Destroy() override{return native_->Destroy();}
    HRESULT STDMETHODCALLTYPE LoadUI(HINSTANCE module,LPCWSTR resource) override{return native_->LoadUI(module,resource);}
    HRESULT STDMETHODCALLTYPE GetView(UINT32 id,REFIID iid,void** output) override{return native_->GetView(id,iid,output);}
    HRESULT STDMETHODCALLTYPE GetUICommandProperty(UINT32 id,REFPROPERTYKEY key,PROPVARIANT* value) override {
        const auto native=translation_->nativeId(id);
        return native?native_->GetUICommandProperty(native,key,value):HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE SetUICommandProperty(UINT32 id,REFPROPERTYKEY key,REFPROPVARIANT value) override {
        if(revision_->retired)return HRESULT_FROM_WIN32(ERROR_RETRY);
        RibbonMutation mutation(revision_);
        const auto native=translation_->nativeId(id);
        return native?native_->SetUICommandProperty(native,key,value):HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE InvalidateUICommand(UINT32 id,UI_INVALIDATIONS flags,const PROPERTYKEY* key) override {
        if(!id)return native_->InvalidateUICommand(0,flags,key);
        const auto native=translation_->nativeId(id);
        return native?native_->InvalidateUICommand(native,flags,key):HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE FlushPendingInvalidations() override{return native_->FlushPendingInvalidations();}
    HRESULT STDMETHODCALLTYPE SetModes(INT32 modes) override{return native_->SetModes(modes);}
    // Framework-wide theme properties are not command identifiers. Preserve
    // their native store and this facade's COM identity across both interfaces.
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override{return properties_?properties_->GetCount(count):E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index,PROPERTYKEY* key) override{return properties_?properties_->GetAt(index,key):E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key,PROPVARIANT* value) override{return properties_?properties_->GetValue(key,value):E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY key,REFPROPVARIANT value) override{return properties_?properties_->SetValue(key,value):E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE Commit() override{return properties_?properties_->Commit():E_NOINTERFACE;}
private:
    std::atomic<ULONG> references_{1};
    ComPtr<IUIFramework> native_;
    ComPtr<IPropertyStore> properties_;
    std::shared_ptr<StockTranslation> translation_;
    std::shared_ptr<RibbonRevision> revision_;
};
class Item final : public IUISimplePropertySet {
public:
    explicit Item(RibbonItem value, bool recent = false, IUIImage* image = nullptr) : value_(std::move(value)), recent_(recent), image_(image) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IUISimplePropertySet)) return E_NOINTERFACE;
        *output = static_cast<IUISimplePropertySet*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (IsEqualPropertyKey(key,UI_PKEY_CommandId)) return InitPropVariantFromUInt32(value_.command,value);
        if (IsEqualPropertyKey(key,UI_PKEY_CommandType)) return InitPropVariantFromUInt32(value_.checkable?UI_COMMANDTYPE_BOOLEAN:UI_COMMANDTYPE_ACTION,value);
        if (IsEqualPropertyKey(key,UI_PKEY_CategoryId)) return InitPropVariantFromUInt32(value_.category,value);
        if (IsEqualPropertyKey(key,UI_PKEY_Label)) return InitPropVariantFromString(value_.label.c_str(),value);
        if(IsEqualPropertyKey(key,UI_PKEY_LabelDescription)&&!value_.description.empty())return InitPropVariantFromString(value_.description.c_str(),value);
        if (IsEqualPropertyKey(key,UI_PKEY_ItemImage) && image_) return UIInitPropertyFromImage(key,image_.Get(),value);
        if (IsEqualPropertyKey(key,UI_PKEY_Pinned) && recent_) return InitPropVariantFromBoolean(value_.pinned,value);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
private:
    std::atomic<ULONG> references_{1};
    RibbonItem value_;
    bool recent_;
    ComPtr<IUIImage> image_;
};
constexpr DWORD maximumSettingsBytes = 64 * 1024;
constexpr std::array<BYTE,8> settingsMagic{'W','E','Q','A','T','\r','\n',0x1a};
constexpr std::size_t settingsHeaderBytes=56;
constexpr std::size_t maximumSettingsFileBytes=maximumSettingsBytes+settingsHeaderBytes+20*4;
constexpr HRESULT changedBinding=HRESULT_FROM_WIN32(ERROR_RETRY);
constexpr HRESULT invalidSettings=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
UINT settingsUInt(std::span<const BYTE> bytes,std::size_t offset) noexcept {
    return static_cast<UINT>(bytes[offset])|(static_cast<UINT>(bytes[offset+1])<<8)|
        (static_cast<UINT>(bytes[offset+2])<<16)|(static_cast<UINT>(bytes[offset+3])<<24);
}
void putSettingsUInt(std::span<BYTE> bytes,std::size_t offset,UINT value) noexcept {
    for(unsigned part=0;part<4;++part)bytes[offset+part]=static_cast<BYTE>(value>>(part*8));
}
HRESULT settingsDigest(std::span<const BYTE> bytes,std::array<BYTE,32>& digest) {
    struct Algorithm {BCRYPT_ALG_HANDLE value=nullptr;~Algorithm(){if(value)BCryptCloseAlgorithmProvider(value,0);}} algorithm;
    auto status=BCryptOpenAlgorithmProvider(&algorithm.value,BCRYPT_SHA256_ALGORITHM,nullptr,0);
    if(status<0)return HRESULT_FROM_NT(status);
    DWORD length=0,written=0;
    status=BCryptGetProperty(algorithm.value,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&written,0);
    if(status<0)return HRESULT_FROM_NT(status);
    if(written!=sizeof(length)||!length)return E_UNEXPECTED;
    std::vector<BYTE> object(length);
    struct Hash {BCRYPT_HASH_HANDLE value=nullptr;~Hash(){if(value)BCryptDestroyHash(value);}} hash;
    status=BCryptCreateHash(algorithm.value,&hash.value,object.data(),length,nullptr,0,0);
    if(status<0)return HRESULT_FROM_NT(status);
    // The digest field is logically zero. Cover header, IDs and opaque payload.
    std::array<BYTE,32> zero{};
    for(const auto part:{bytes.first(24),std::span<const BYTE>(zero),bytes.subspan(settingsHeaderBytes)}) {
        status=BCryptHashData(hash.value,const_cast<PUCHAR>(part.data()),static_cast<ULONG>(part.size()),0);
        if(status<0)return HRESULT_FROM_NT(status);
    }
    status=BCryptFinishHash(hash.value,digest.data(),static_cast<ULONG>(digest.size()),0);
    return status<0?HRESULT_FROM_NT(status):S_OK;
}
struct SettingsFile {
    std::span<const BYTE> native;
    std::vector<UINT> order;
    bool envelope=false;
};
HRESULT decodeSettings(std::span<const BYTE> bytes,RibbonLayout layout,SettingsFile& output) {
    if(bytes.empty()||bytes.size()>maximumSettingsFileBytes)return invalidSettings;
    const bool marked=bytes.size()>=5&&std::equal(settingsMagic.begin(),settingsMagic.begin()+5,bytes.begin());
    if(!marked) {
        if(bytes.size()>maximumSettingsBytes)return invalidSettings;
        output.native=bytes;return S_OK; // Native itself validates legacy opaque bytes.
    }
    if(bytes.size()<settingsHeaderBytes||!std::equal(settingsMagic.begin(),settingsMagic.end(),bytes.begin()))return invalidSettings;
    const auto version=settingsUInt(bytes,8),savedLayout=settingsUInt(bytes,12),count=settingsUInt(bytes,16),length=settingsUInt(bytes,20);
    if(version!=1||savedLayout>1||savedLayout!=static_cast<UINT>(layout)||count>20||!length||length>maximumSettingsBytes)
        return invalidSettings;
    const std::size_t payload=settingsHeaderBytes+static_cast<std::size_t>(count)*4;
    if(bytes.size()!=payload+length)return invalidSettings; // Exact size rejects overflow, truncation and trailing bytes.
    std::vector<UINT> order;order.reserve(count);
    for(UINT index=0;index<count;++index) {
        const auto id=settingsUInt(bytes,settingsHeaderBytes+index*4);
        if(!id||std::find(order.begin(),order.end(),id)!=order.end())return invalidSettings;
        order.push_back(id);
    }
    std::array<BYTE,32> digest{};const auto hr=settingsDigest(bytes,digest);if(hr!=S_OK)return hr;
    if(!std::equal(digest.begin(),digest.end(),bytes.begin()+24))return invalidSettings;
    output.native=bytes.subspan(payload,length);output.order=std::move(order);output.envelope=true;return S_OK;
}
HRESULT encodeSettings(RibbonLayout layout,std::span<const UINT> order,std::span<const BYTE> native,std::vector<BYTE>& output) {
    if(order.size()>20||native.empty()||native.size()>maximumSettingsBytes)return invalidSettings;
    std::vector<BYTE> bytes(settingsHeaderBytes+order.size()*4+native.size());
    std::copy(settingsMagic.begin(),settingsMagic.end(),bytes.begin());
    putSettingsUInt(bytes,8,1);putSettingsUInt(bytes,12,static_cast<UINT>(layout));
    putSettingsUInt(bytes,16,static_cast<UINT>(order.size()));putSettingsUInt(bytes,20,static_cast<UINT>(native.size()));
    for(std::size_t index=0;index<order.size();++index) {
        if(!order[index]||std::find(order.begin(),order.begin()+static_cast<std::ptrdiff_t>(index),order[index])!=order.begin()+static_cast<std::ptrdiff_t>(index))return invalidSettings;
        putSettingsUInt(bytes,settingsHeaderBytes+index*4,order[index]);
    }
    std::copy(native.begin(),native.end(),bytes.begin()+static_cast<std::ptrdiff_t>(settingsHeaderBytes+order.size()*4));
    std::array<BYTE,32> digest{};const auto hr=settingsDigest(bytes,digest);if(hr!=S_OK)return hr;
    std::copy(digest.begin(),digest.end(),bytes.begin()+24);output=std::move(bytes);return S_OK;
}
HRESULT settingsStreamBytes(IStream* stream,std::vector<BYTE>& output) {
    STATSTG stat{};auto hr=stream->Stat(&stat,STATFLAG_NONAME);if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
    if(!stat.cbSize.QuadPart||stat.cbSize.QuadPart>maximumSettingsBytes)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    HGLOBAL memory=nullptr;hr=GetHGlobalFromStream(stream,&memory);if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
    struct Lock {HGLOBAL memory;const BYTE* bytes;~Lock(){if(bytes)GlobalUnlock(memory);}} lock{memory,static_cast<const BYTE*>(GlobalLock(memory))};
    if(!lock.bytes)return E_OUTOFMEMORY;
    output.assign(lock.bytes,lock.bytes+static_cast<std::size_t>(stat.cbSize.QuadPart));return S_OK;
}
HRESULT readFile(const std::filesystem::path& path, std::vector<BYTE>& output) {
    const HANDLE file = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    struct CloseFile {HANDLE value;~CloseFile(){CloseHandle(value);}} close{file};
    LARGE_INTEGER size{};
    HRESULT result = S_OK;
    if (!GetFileSizeEx(file,&size)) result = HRESULT_FROM_WIN32(GetLastError());
    else if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(maximumSettingsFileBytes)) result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    else {
        std::vector<BYTE> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)) result = HRESULT_FROM_WIN32(GetLastError());
        else if (read != bytes.size()) result = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        else output = std::move(bytes);
    }
    return result;
}
}

struct RibbonQuickAccessSnapshot::Impl {
    const NativeRibbon* owner=nullptr;
    const void* ribbonState=nullptr;
    DWORD thread=0;
    HWND window=nullptr;
    std::uint64_t generation=0,revision=0;
    ComPtr<IUIFramework> framework;
    UINT nativeQuickAccess=0;
    ComPtr<IUnknown> viewIdentity;
    ComPtr<IUICollection> collection;
    ComPtr<IUnknown> collectionIdentity;
    ComPtr<IPropertyStore> viewProperties;
    std::vector<ComPtr<IUnknown>> rows,identities;
    std::vector<RibbonQuickAccessItem> items;
    ULONG dock=UI_CONTROLDOCK_TOP;
    bool minimized=false;
    struct Property {
        PROPVARIANT value{};
        HRESULT result=E_PENDING;
        ComPtr<IUnknown> identity;
        Property()=default;
        Property(const Property&)=delete;
        Property& operator=(const Property&)=delete;
        Property(Property&& other)noexcept:value(other.value),result(other.result),identity(std::move(other.identity)){PropVariantInit(&other.value);}
        Property& operator=(Property&& other)noexcept {
            if(this!=&other){PropVariantClear(&value);value=other.value;PropVariantInit(&other.value);result=other.result;identity=std::move(other.identity);}return *this;
        }
        ~Property(){PropVariantClear(&value);}
    };
    // Preserve and fence the documented row metadata, including failure/VT
    // provenance and canonical object-valued properties. Rows remain opaque.
    static constexpr std::size_t propertyCount=12;
    static inline const std::array<const PROPERTYKEY*,propertyCount> propertyKeys{&UI_PKEY_CommandId,&UI_PKEY_CommandType,&UI_PKEY_Label,
        &UI_PKEY_LabelDescription,&UI_PKEY_Enabled,&UI_PKEY_Pinned,&UI_PKEY_CategoryId,&UI_PKEY_Keytip,
        &UI_PKEY_TooltipTitle,&UI_PKEY_TooltipDescription,&UI_PKEY_SmallImage,&UI_PKEY_LargeImage};
    using Properties=std::array<Property,propertyCount>;
    std::vector<Properties> properties;
    template<class Current>
    static HRESULT readProperties(IUnknown* row,Properties& output,Current&& current) {
        ComPtr<IUISimplePropertySet> set;const auto query=row->QueryInterface(IID_PPV_ARGS(&set));
        if(!current())return changedBinding;
        for(std::size_t index=0;index<propertyKeys.size();++index) {
            auto& property=output[index];property.result=query==S_OK&&set?set->GetValue(*propertyKeys[index],&property.value):query;
            if(!current())return changedBinding;
            if(property.result==S_OK&&property.value.vt==VT_UNKNOWN&&property.value.punkVal) {
                const auto hr=property.value.punkVal->QueryInterface(IID_PPV_ARGS(&property.identity));
                if(!current())return changedBinding;
                if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
            }
        }
        return S_OK;
    }
    static bool sameProperty(const Property& left,const Property& right) noexcept {
        if(left.result!=right.result||left.value.vt!=right.value.vt)return false;
        if(left.value.vt==VT_UNKNOWN)return left.identity.Get()==right.identity.Get()&&
            (left.identity?true:left.value.punkVal==right.value.punkVal)&&
            (left.value.punkVal==nullptr)==(right.value.punkVal==nullptr);
        if(left.value.vt==VT_BOOL)return left.value.boolVal==right.value.boolVal;
        if(left.value.vt==VT_LPWSTR)return (left.value.pwszVal==nullptr)==(right.value.pwszVal==nullptr)&&
            (!left.value.pwszVal||std::wcscmp(left.value.pwszVal,right.value.pwszVal)==0);
        if(left.value.vt==VT_BSTR)return (left.value.bstrVal==nullptr)==(right.value.bstrVal==nullptr)&&
            SysStringLen(left.value.bstrVal)==SysStringLen(right.value.bstrVal)&&
            (!left.value.bstrVal||std::memcmp(left.value.bstrVal,right.value.bstrVal,SysStringLen(left.value.bstrVal)*sizeof(wchar_t))==0);
        return PropVariantCompareEx(left.value,right.value,PVCU_DEFAULT,PVCF_USESTRCMPC)==0;
    }

    static HRESULT readCommand(IUnknown* row,UINT& nativeCommand) {
        ComPtr<IUISimplePropertySet> properties;
        auto hr=row->QueryInterface(IID_PPV_ARGS(&properties));
        if(hr!=S_OK)return hr;
        if(!properties)return E_UNEXPECTED;
        Variant value;hr=properties->GetValue(UI_PKEY_CommandId,&value.value);
        if(hr!=S_OK)return hr;
        ULONG command=0;hr=PropVariantToUInt32(value.value,&command);
        if(hr==S_OK)nativeCommand=command;
        return hr;
    }
    template<class Current>
    HRESULT matches(const std::vector<ComPtr<IUnknown>>& expected,Current&& current,UINT addedCommand=0,const Impl* extra=nullptr) const {
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        Variant source;auto hr=framework->GetUICommandProperty(nativeQuickAccess,UI_PKEY_ItemsSource,&source.value);
        if(!current())return changedBinding;
        if(hr!=S_OK||source.value.vt!=VT_UNKNOWN||!source.value.punkVal)return FAILED(hr)?hr:changedBinding;
        ComPtr<IUnknown> actualCollection;hr=source.value.punkVal->QueryInterface(IID_PPV_ARGS(&actualCollection));
        if(!current())return changedBinding;
        if(hr!=S_OK||actualCollection.Get()!=collectionIdentity.Get())return changedBinding;
        ComPtr<IUIRibbon> actualRibbon;hr=framework->GetView(0,IID_PPV_ARGS(&actualRibbon));
        if(!current())return changedBinding;
        if(hr!=S_OK||!actualRibbon)return changedBinding;
        ComPtr<IUnknown> actualView;hr=actualRibbon.As(&actualView);
        if(!current())return changedBinding;
        if(hr!=S_OK||actualView.Get()!=viewIdentity.Get())return changedBinding;
        UINT count=0;hr=collection->GetCount(&count);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        if(count!=expected.size())return HRESULT_FROM_WIN32(ERROR_RETRY);
        for(UINT index=0;index<count;++index) {
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            ComPtr<IUnknown> row,identity;
            hr=collection->GetItem(index,&row);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(hr!=S_OK||!row)return FAILED(hr)?hr:E_UNEXPECTED;
            hr=row.As(&identity);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(hr!=S_OK||!identity)return FAILED(hr)?hr:E_UNEXPECTED;
            if(identity.Get()!=expected[index].Get())return HRESULT_FROM_WIN32(ERROR_RETRY);
            // Reordering changes collection indices, never the binding of a
            // retained original object. Match its original snapshot identity.
            const auto original=std::find_if(identities.begin(),identities.end(),[&](const auto& retained){
                return retained.Get()==identity.Get();});
            UINT command=0;const auto read=readCommand(row.Get(),command);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            const auto extraOriginal=extra?std::find_if(extra->identities.begin(),extra->identities.end(),[&](const auto& retained){
                return retained.Get()==identity.Get();}):identities.end();
            const Impl* binding=nullptr;std::size_t originalIndex=0;
            if(extra&&extraOriginal!=extra->identities.end()){binding=extra;originalIndex=static_cast<std::size_t>(extraOriginal-extra->identities.begin());}
            else if(original!=identities.end()){binding=this;originalIndex=static_cast<std::size_t>(original-identities.begin());}
            if(binding) {
                if(read!=binding->items[originalIndex].commandRead||
                   (read==S_OK&&command!=binding->items[originalIndex].nativeCommand))return HRESULT_FROM_WIN32(ERROR_RETRY);
                Properties observed;hr=readProperties(row.Get(),observed,current);if(hr!=S_OK)return hr;
                for(std::size_t property=0;property<propertyKeys.size();++property)
                    if(!sameProperty(binding->properties[originalIndex][property],observed[property]))return changedBinding;
            } else if(!addedCommand||read!=S_OK||command!=addedCommand) {
                return HRESULT_FROM_WIN32(ERROR_RETRY);
            }
        }
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        UINT finalCount=0;hr=collection->GetCount(&finalCount);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        if(finalCount!=count)return HRESULT_FROM_WIN32(ERROR_RETRY);
        Variant value;hr=viewProperties->GetValue(UI_PKEY_QuickAccessToolbarDock,&value.value);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        ULONG actualDock=0;hr=PropVariantToUInt32(value.value,&actualDock);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        if(actualDock!=dock)return changedBinding;
        Variant minimizedValue;hr=viewProperties->GetValue(UI_PKEY_Minimized,&minimizedValue.value);
        if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        BOOL actualMinimized=FALSE;hr=PropVariantToBoolean(minimizedValue.value,&actualMinimized);
        if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        return (actualMinimized!=FALSE)==minimized?S_OK:changedBinding;
    }
};
RibbonQuickAccessSnapshot::RibbonQuickAccessSnapshot()=default;
RibbonQuickAccessSnapshot::~RibbonQuickAccessSnapshot()=default;
RibbonQuickAccessSnapshot::RibbonQuickAccessSnapshot(RibbonQuickAccessSnapshot&&) noexcept=default;
RibbonQuickAccessSnapshot& RibbonQuickAccessSnapshot::operator=(RibbonQuickAccessSnapshot&&) noexcept=default;
std::span<const RibbonQuickAccessItem> RibbonQuickAccessSnapshot::items() const noexcept {
    return impl_?std::span<const RibbonQuickAccessItem>(impl_->items):std::span<const RibbonQuickAccessItem>{};
}
bool RibbonQuickAccessSnapshot::belowRibbon() const noexcept {return impl_&&impl_->dock==UI_CONTROLDOCK_BOTTOM;}

struct NativeRibbon::Impl : std::enable_shared_from_this<NativeRibbon::Impl> {
    enum class LabelSource { NativeProvider, AuthoredFallback, EnglishPresentation };
    struct Metadata {
        std::wstring label,description,icon;
        LabelSource labelSource=LabelSource::AuthoredFallback;
    };
    HWND window=nullptr;
    DWORD thread=0;
    std::shared_ptr<RibbonRevision> revision=std::make_shared<RibbonRevision>();
    std::shared_ptr<std::atomic<std::uint64_t>> callbackEpoch;
    std::uint64_t shutdownRecentItemsEpoch=0;
    IUIFramework* shutdownRecentItemsFramework=nullptr; // Borrowed only inside reset's retained native stack.
    bool shutdownRecentItems=false,shutdownRecentItemsStarted=false;
    std::vector<bool> shutdownRecentPins;
    std::function<HRESULT(UINT,bool)> shutdownPinItem;
    UINT height=0;
    bool computerMode=false;
    bool networkMode=false;
    UINT driveType=DRIVE_UNKNOWN;
    RibbonFeatures features;
    RibbonContext contexts=RibbonContext::None;
    bool activeContext=false;
    bool englishPresentation=true;
    RibbonLayout layout=RibbonLayout::Authored;
    HRESULT stockStatus=E_NOTIMPL;
    HMODULE stockModule=nullptr;
    RibbonCallbacks callbacks;
    ComPtr<IUIFramework> framework;
    ComPtr<IUIFramework> publicFramework;
    ComPtr<IUIRibbon> ribbon;
    ComPtr<IUIImageFromBitmap> images;
    ComPtr<IWICImagingFactory> imaging;
    std::map<UINT,UI_COMMANDTYPE> commandTypes;
    std::map<UINT,UI_COMMANDTYPE> nativeCommandTypes;
    bool readOnlyMenuExpansion=false;
    UINT menuExpansionExecutionAttempts=0;
    std::map<UINT,Metadata> metadata;
    // Resource properties are not GetUICommandProperty-readable. Copy their
    // initial native callback values while valid; never mistake a later value
    // supplied by this host for an installed localized resource.
    std::set<UINT> observedNativeLabels,observedNativeTooltips;
    std::map<UINT,std::wstring> nativeLabels,nativeTooltips;
    std::array<std::wstring,8> viewTitles;
    std::map<std::pair<UINT,UINT>,ComPtr<IUIImage>> imageCache;
    std::map<std::pair<std::wstring,UINT>,ComPtr<IUIImage>> itemImageCache;
    std::map<UINT,std::wstring> defaultIcons;
    struct DynamicCommand {UINT parent,index,nativeParent;RibbonItem item;};
    std::map<UINT,DynamicCommand> dynamicCommands;
    std::map<UINT,std::vector<UINT>> collectionInvocationIndices;
    std::map<UI_COMMANDTYPE,std::vector<UINT>> availableDynamicCommands;
    std::vector<bool> recentPins;
    std::set<UINT> requestedCollections;
    std::map<UINT,RibbonCollectionReadback> collectionReads;
    struct Invalidation {UINT command;UI_INVALIDATIONS flags;PROPERTYKEY key{};bool hasKey=false;};
    std::vector<Invalidation> deferredInvalidations;
    UINT propertyDepth=0;
    UINT invalidationMessage=0;
    bool invalidationPosted=false,subclassAttached=false;
    std::shared_ptr<StockTranslation> translation=std::make_shared<StockTranslation>();
    UINT nextDynamicCommand=0x9000;
    ~Impl(){detachSubclass();if(stockModule)FreeLibrary(stockModule);}
    void detachSubclass() noexcept {
        if(subclassAttached&&IsWindow(window))RemoveWindowSubclass(window,subclassProcedure,reinterpret_cast<UINT_PTR>(this));
        subclassAttached=false;invalidationPosted=false;
    }
    void postInvalidations() noexcept {
        if(!propertyDepth&&!invalidationPosted&&!deferredInvalidations.empty()&&subclassAttached)
            invalidationPosted=PostMessageW(window,invalidationMessage,reinterpret_cast<WPARAM>(this),0)!=FALSE;
    }
    HRESULT requestInvalidation(UINT command,UI_INVALIDATIONS flags,const PROPERTYKEY* key=nullptr) {
        const auto lifetime=shared_from_this();if(revision->retired)return changedBinding;
        if(!framework)return E_UNEXPECTED;
        if(!propertyDepth){const auto retained=framework;const auto result=retained->InvalidateUICommand(command,flags,key);return revision->retired?changedBinding:result;}
        // UpdateProperty must return before calling the Ribbon framework again.
        // The queue owns property keys; no borrowed callback pointer survives.
        const auto found=std::find_if(deferredInvalidations.begin(),deferredInvalidations.end(),[&](const auto& entry) {
            return entry.command==command&&entry.flags==flags&&entry.hasKey==(key!=nullptr)&&
                (!key||IsEqualPropertyKey(entry.key,*key));
        });
        if(found==deferredInvalidations.end())deferredInvalidations.push_back({command,flags,key?*key:PROPERTYKEY{},key!=nullptr});
        return S_OK;
    }
    HRESULT drainInvalidations() {
        const auto lifetime=shared_from_this();if(revision->retired)return changedBinding;
        if(propertyDepth)return E_PENDING;
        invalidationPosted=false;
        auto pending=std::move(deferredInvalidations);deferredInvalidations.clear();
        HRESULT result=S_OK;
        for(const auto& entry:pending) {
            const auto hr=requestInvalidation(entry.command,entry.flags,entry.hasKey?&entry.key:nullptr);
            if(revision->retired)return changedBinding;
            if(FAILED(hr)&&SUCCEEDED(result))result=hr;
        }
        postInvalidations();return result;
    }
    static LRESULT CALLBACK subclassProcedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR data) {
        auto& owner=*reinterpret_cast<Impl*>(data);
        const auto lifetime=owner.shared_from_this();
        if(message==owner.invalidationMessage&&wParam==reinterpret_cast<WPARAM>(&owner)) {
            // Nested message pumps inside a provider cannot bypass the return
            // boundary. The outer property callback posts again on its exit.
            owner.invalidationPosted=false;
            if(!owner.propertyDepth)try{owner.drainInvalidations();}catch(...){}
            return 0;
        }
        if(message==WM_NCDESTROY){owner.revision->windowDestroyed=true;++owner.revision->value;owner.detachSubclass();}
        return DefSubclassProc(window,message,wParam,lParam);
    }
    struct PropertyCallback {
        Impl& owner;
        explicit PropertyCallback(Impl& value):owner(value){++owner.propertyDepth;}
        ~PropertyCallback(){--owner.propertyDepth;if(!owner.revision->retired)owner.postInvalidations();}
    };
    std::vector<RibbonItem> collectionItems(UINT command) {
        std::vector<RibbonItem> items;
        if(command==RibbonLayoutGallery){for(UINT index=0;index<8;++index)items.push_back({ViewFirst+index,viewTitles[index],false});}
        else if(command==RibbonShareGallery)items.push_back({RibbonSpecificPeople,metadata[RibbonSpecificPeople].label,false});
        else if(callbacks.items)items=callbacks.items(command);
        return items;
    }
    UINT nativeId(UINT command) const noexcept {return layout==RibbonLayout::InstalledWindows10?translation->nativeId(command):command;}
    UINT applicationId(UINT command) const noexcept {return dynamicCommands.contains(command)?command:layout==RibbonLayout::InstalledWindows10?stockApplicationId(command):command;}
    void updateTemplateAliases() {
        const bool noBitLocker=features.editionStatus==S_OK&&!features.bitLocker;
        const bool noMediaFoundation=!features.mediaFoundation;
        const bool noBurn=!features.discBurning;
        const bool noCleanup=!features.diskCleanup||(driveType!=DRIVE_UNKNOWN&&driveType!=DRIVE_NO_ROOT_DIR&&driveType!=DRIVE_FIXED);
        const auto pair=[&](UINT context,UINT tab,UINT nativeContext,UINT nativeTab) {
            translation->overrides[context]=nativeContext;translation->overrides[tab]=nativeTab;
        };
        UINT driveContext=0x703,driveTab=0x1c30;
        if(noBitLocker&&noBurn){driveContext=0x709;driveTab=0x1c33;}
        else if(noBitLocker&&noCleanup){driveContext=0x70e;driveTab=0x1c34;}
        else if(noBitLocker){driveContext=0x707;driveTab=0x1c31;}
        else if(noCleanup){driveContext=0x708;driveTab=0x1c32;}
        pair(RibbonDriveContext,RibbonDriveTab,driveContext,driveTab);
        pair(RibbonPictureContext,RibbonPictureTab,noMediaFoundation?0x70c:0x700,noMediaFoundation?0x1c01:0x1c00);
        pair(RibbonMusicContext,RibbonMusicTab,noMediaFoundation?0x70b:0x701,noMediaFoundation?0x1c11:0x1c10);
        pair(RibbonVideoContext,RibbonVideoTab,noMediaFoundation?0x70a:0x702,noMediaFoundation?0x1c21:0x1c20);
        pair(RibbonDiscImageContext,RibbonDiscImageTab,noBurn?0x70d:0x704,noBurn?0x1c41:0x1c40);
    }
    UINT homeModes() const noexcept {return 0xa1|(features.discBurning?0x20000:0x40000);}
    UINT computerModes() const noexcept {return 4|(features.mediaFoundation?0x2000:0x4000);}

    bool shutdownRecentItemsCurrent() const noexcept {
        if(GetCurrentThreadId()!=thread)return false;
        DWORD process=0;
        return revision->retired&&shutdownRecentItems&&callbackEpoch&&callbackEpoch->load()==shutdownRecentItemsEpoch&&
            framework.Get()==shutdownRecentItemsFramework&&shutdownRecentItemsFramework&&
            !revision->windowDestroyed&&IsWindow(window)&&GetWindowThreadProcessId(window,&process)==thread&&
            process==GetCurrentProcessId();
    }
    HRESULT executeShutdownRecentItems(const PROPVARIANT* value) {
        // The only retired Execute authorized by reset is one complete native
        // RecentItems commit from this captured old framework, on its creator.
        if(!shutdownRecentItemsCurrent()||shutdownRecentItemsStarted)return changedBinding;
        shutdownRecentItemsStarted=true; // Nested/replayed commits cannot restart it.
        const auto initialPins=shutdownRecentPins;const auto pinItem=shutdownPinItem;
        if(!shutdownRecentItemsCurrent())return changedBinding;
        if(!value||value->vt!=(VT_ARRAY|VT_UNKNOWN)||!value->parray||SafeArrayGetDim(value->parray)!=1)return E_INVALIDARG;
        LONG first=0,last=-1;
        if(!shutdownRecentItemsCurrent())return changedBinding;
        auto hr=SafeArrayGetLBound(value->parray,1,&first);
        if(!shutdownRecentItemsCurrent())return changedBinding;
        if(FAILED(hr))return hr;
        hr=SafeArrayGetUBound(value->parray,1,&last);
        if(!shutdownRecentItemsCurrent())return changedBinding;
        if(FAILED(hr))return hr;
        const auto count=static_cast<std::int64_t>(last)-static_cast<std::int64_t>(first)+1;
        if(count<0||count>64||static_cast<std::uint64_t>(count)<initialPins.size())return E_INVALIDARG;
        // Decode every originally owned row before invoking any host code.
        // Remaining native padding is intentionally not a host model row.
        std::vector<bool> decoded(initialPins.size());
        for(size_t index=0;index<initialPins.size();++index) {
            if(!shutdownRecentItemsCurrent())return changedBinding;
            LONG nativeIndex=first+static_cast<LONG>(index);
            ComPtr<IUnknown> raw;ComPtr<IUISimplePropertySet> item;Variant pin;
            hr=SafeArrayGetElement(value->parray,&nativeIndex,raw.GetAddressOf());
            if(!shutdownRecentItemsCurrent())return changedBinding;
            if(FAILED(hr))return hr;
            if(!raw)return E_INVALIDARG;
            hr=raw.As(&item);
            if(!shutdownRecentItemsCurrent())return changedBinding;
            if(FAILED(hr))return hr;
            hr=item->GetValue(UI_PKEY_Pinned,&pin.value);
            if(!shutdownRecentItemsCurrent())return changedBinding;
            if(FAILED(hr))return hr;
            BOOL pinned=FALSE;hr=PropVariantToBoolean(pin.value,&pinned);
            if(!shutdownRecentItemsCurrent())return changedBinding;
            if(FAILED(hr))return hr;
            decoded[index]=pinned!=FALSE;
        }
        HRESULT result=S_OK;
        for(size_t index=0;index<initialPins.size();++index) {
            if(!shutdownRecentItemsCurrent())return changedBinding;
            if(initialPins[index]==decoded[index]||!pinItem)continue;
            hr=pinItem(static_cast<UINT>(index),decoded[index]);
            if(!shutdownRecentItemsCurrent())return changedBinding;
            if(FAILED(hr)&&SUCCEEDED(result))result=hr;
        }
        return result;
    }

    struct Handler final : IUIApplication,IUICommandHandler {
        explicit Handler(Impl& owner): owner_(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
            if(!output) return E_POINTER; *output=nullptr;
            if(iid==IID_IUnknown||iid==__uuidof(IUIApplication)) *output=static_cast<IUIApplication*>(this);
            else if(iid==__uuidof(IUICommandHandler)) *output=static_cast<IUICommandHandler*>(this);
            else return E_NOINTERFACE;
            AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
        ULONG STDMETHODCALLTYPE Release() override {const auto count=--references_;if(!count)delete this;return count;}
        HRESULT STDMETHODCALLTYPE OnViewChanged(UINT32,UI_VIEWTYPE type,IUnknown* view,UI_VIEWVERB verb,INT32 reason) override {
            if(type!=UI_VIEWTYPE_RIBBON)return E_NOTIMPL;
            const auto lifetime=owner_.shared_from_this();
            try {
                if(verb==UI_VIEWVERB_ERROR)return reason;
                if(verb==UI_VIEWVERB_CREATE||verb==UI_VIEWVERB_SIZE){
                    ComPtr<IUIRibbon> nativeView;
                    const auto hr=view?view->QueryInterface(IID_PPV_ARGS(&nativeView)):E_POINTER;
                    if(owner_.revision->retired)return changedBinding;
                    if(FAILED(hr))return hr;
                    UINT nativeHeight=0;const auto result=nativeView->GetHeight(&nativeHeight);
                    if(owner_.revision->retired)return changedBinding;
                    if(FAILED(result))return result;
                    owner_.ribbon=nativeView;owner_.height=nativeHeight;
                    if(owner_.callbacks.heightChanged)owner_.callbacks.heightChanged(nativeHeight);
                    if(owner_.revision->retired)return changedBinding;
                }else if(verb==UI_VIEWVERB_DESTROY){owner_.height=0;owner_.ribbon.Reset();}
                return S_OK;
            } catch(...) {return E_FAIL;}
        }
        HRESULT STDMETHODCALLTYPE OnCreateUICommand(UINT32 id,UI_COMMANDTYPE type,IUICommandHandler** output) override {
            if(!output)return E_POINTER;*output=nullptr;
            try {owner_.nativeCommandTypes[id]=type;const auto command=owner_.applicationId(id);if(command)owner_.commandTypes[command]=type;
                if(command){auto& read=owner_.collectionReads[command];read.registered=true;read.nativeType=type;}
                *output=static_cast<IUICommandHandler*>(this);AddRef();return S_OK;}
            catch(...) {return E_OUTOFMEMORY;}
        }
        HRESULT STDMETHODCALLTYPE OnDestroyUICommand(UINT32 id,UI_COMMANDTYPE,IUICommandHandler*) override {
            owner_.observedNativeLabels.erase(id);owner_.observedNativeTooltips.erase(id);
            owner_.nativeLabels.erase(id);owner_.nativeTooltips.erase(id);return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Execute(UINT32 id,UI_EXECUTIONVERB verb,const PROPERTYKEY* key,const PROPVARIANT* value,IUISimplePropertySet* properties) override {
            if(verb!=UI_EXECUTIONVERB_EXECUTE)return S_OK;
            if(GetCurrentThreadId()!=owner_.thread)return RPC_E_WRONG_THREAD;
            const auto lifetime=owner_.shared_from_this();
            if(owner_.revision->retired) {
                if(owner_.readOnlyMenuExpansion||id!=owner_.nativeId(RibbonFrequentPlaces)||owner_.applicationId(id)!=RibbonFrequentPlaces||
                   !key||!IsEqualPropertyKey(*key,UI_PKEY_RecentItems))return changedBinding;
                try{return owner_.executeShutdownRecentItems(value);}catch(...){return E_FAIL;}
            }
            if(owner_.readOnlyMenuExpansion){++owner_.menuExpansionExecutionAttempts;return E_ACCESSDENIED;}
            RibbonMutation mutation(owner_.revision);
            try {
                const auto originalId=id;
                if(const auto dynamic=owner_.dynamicCommands.find(id);dynamic!=owner_.dynamicCommands.end())
                    return dynamic->second.item.enabled&&owner_.callbacks.executeItem?
                        owner_.callbacks.executeItem(dynamic->second.parent,dynamic->second.index):E_ACCESSDENIED;
                id=owner_.applicationId(id);if(!id)return E_NOTIMPL;
                if(id==RibbonFrequentPlaces&&key&&IsEqualPropertyKey(*key,UI_PKEY_RecentItems)) {
                    if(!value||value->vt!=(VT_ARRAY|VT_UNKNOWN)||!value->parray)return E_INVALIDARG;
                    LONG first=0,last=-1;auto hr=SafeArrayGetLBound(value->parray,1,&first);
                    if(SUCCEEDED(hr))hr=SafeArrayGetUBound(value->parray,1,&last);
                    if(FAILED(hr)||last-first>=64)return E_INVALIDARG;
                    const auto initialPins=owner_.recentPins;
                    if(static_cast<size_t>(last-first+1)<initialPins.size())return E_INVALIDARG;
                    HRESULT result=S_OK;
                    for(LONG index=first;index<first+static_cast<LONG>(initialPins.size());++index) {
                        ComPtr<IUnknown> raw;ComPtr<IUISimplePropertySet> item;Variant pin;
                        hr=SafeArrayGetElement(value->parray,&index,raw.GetAddressOf());
                        if(SUCCEEDED(hr))hr=raw.As(&item);
                        if(SUCCEEDED(hr))hr=item->GetValue(UI_PKEY_Pinned,&pin.value);
                        BOOL pinned=FALSE;if(SUCCEEDED(hr))hr=PropVariantToBoolean(pin.value,&pinned);
                        if(SUCCEEDED(hr)&&initialPins[static_cast<size_t>(index-first)]!=(pinned!=FALSE)&&owner_.callbacks.pinItem)
                            hr=owner_.callbacks.pinItem(static_cast<UINT>(index-first),pinned!=FALSE);
                        if(owner_.revision->retired)return changedBinding;
                        if(FAILED(hr)&&SUCCEEDED(result))result=hr;
                    }
                    return result;
                }
                if(id==RibbonFrequentPlaces&&key&&IsEqualPropertyKey(*key,UI_PKEY_Pinned)) {
                    BOOL pinned=FALSE;ULONG index=0;Variant selected;
                    auto hr=value?PropVariantToBoolean(*value,&pinned):E_POINTER;
                    if(SUCCEEDED(hr))hr=properties?properties->GetValue(UI_PKEY_SelectedItem,&selected.value):E_POINTER;
                    if(SUCCEEDED(hr))hr=PropVariantToUInt32(selected.value,&index);
                    return SUCCEEDED(hr)&&owner_.callbacks.pinItem?owner_.callbacks.pinItem(index,pinned!=FALSE):FAILED(hr)?hr:E_NOTIMPL;
                }
                if(id==RibbonLayoutGallery&&key&&IsEqualPropertyKey(*key,UI_PKEY_SelectedItem)){
                    ULONG selected=0;const auto hr=value?PropVariantToUInt32(*value,&selected):E_POINTER;
                    if(FAILED(hr)||selected>=8)return E_INVALIDARG;id=ViewFirst+selected;
                }
                if(id==RibbonShareGallery&&key&&IsEqualPropertyKey(*key,UI_PKEY_SelectedItem)) {
                    ULONG selected=0; const auto hr=value?PropVariantToUInt32(*value,&selected):E_POINTER;
                    if(FAILED(hr)||selected!=0)return E_INVALIDARG;
                    id=RibbonSpecificPeople;
                }
                if((id==RibbonNewMenu||id==RibbonExtractToGallery||id==RibbonFrequentPlaces||id==RecentSearches||id==SearchDateMenu||id==SearchKindMenu||id==SearchSizeMenu||id==RibbonSearchOtherProperties)&&key&&IsEqualPropertyKey(*key,UI_PKEY_SelectedItem)){
                    ULONG selected=0;const auto hr=value?PropVariantToUInt32(*value,&selected):E_POINTER;
                    if(FAILED(hr)||selected>=4096)return E_INVALIDARG;
                    if(id==RibbonExtractToGallery) {
                        const auto indices=owner_.collectionInvocationIndices.find(originalId);
                        if(indices==owner_.collectionInvocationIndices.end()||selected>=indices->second.size())return E_INVALIDARG;
                        selected=indices->second[selected];
                    }
                    return owner_.callbacks.executeItem?owner_.callbacks.executeItem(id,selected):E_NOTIMPL;
                }
                if(id==RibbonHelpButton)id=RibbonHelp;
                if(key&&IsEqualPropertyKey(*key,UI_PKEY_SelectedItem)&&value&&owner_.callbacks.executeItem) {
                    ULONG selected=0;const auto hr=PropVariantToUInt32(*value,&selected);
                    if(FAILED(hr))return hr;
                    if(const auto indices=owner_.collectionInvocationIndices.find(originalId);indices!=owner_.collectionInvocationIndices.end()) {
                        if(selected>=indices->second.size())return E_INVALIDARG;
                        selected=indices->second[selected];
                    }
                    return owner_.callbacks.executeItem(id,selected);
                }
                if(owner_.callbacks.execute)return owner_.callbacks.execute(id);
                return E_NOTIMPL;
            }catch(...){return E_FAIL;}
        }
        HRESULT STDMETHODCALLTYPE UpdateProperty(UINT32 id,REFPROPERTYKEY key,const PROPVARIANT* current,PROPVARIANT* value) override {
            if(!value)return E_POINTER;
            PropVariantInit(value);
            const auto lifetime=owner_.shared_from_this();if(owner_.revision->retired)return changedBinding;
            PropertyCallback callback(owner_);
            try {
                const auto originalId=id;
                const auto dynamic=owner_.dynamicCommands.find(id);
                if(dynamic!=owner_.dynamicCommands.end()) {
                    const auto& item=dynamic->second.item;
                    if(IsEqualPropertyKey(key,UI_PKEY_Enabled)) {
                        const auto parent=owner_.callbacks.query?owner_.callbacks.query(dynamic->second.parent):RibbonCommandState{};
                        if(owner_.revision->retired)return changedBinding;
                        return InitPropVariantFromBoolean(parent.enabled&&item.enabled,value);
                    }
                    if(IsEqualPropertyKey(key,UI_PKEY_BooleanValue))return InitPropVariantFromBoolean(item.checked,value);
                    if(IsEqualPropertyKey(key,UI_PKEY_Label)||IsEqualPropertyKey(key,UI_PKEY_TooltipTitle))return InitPropVariantFromString(item.label.c_str(),value);
                    if(IsEqualPropertyKey(key,UI_PKEY_TooltipDescription))return InitPropVariantFromString(item.description.c_str(),value);
                    if(IsEqualPropertyKey(key,UI_PKEY_SmallImage)||IsEqualPropertyKey(key,UI_PKEY_LargeImage)) {
                        ComPtr<IUIImage> image;const auto hr=owner_.imageSpec(item.image,IsEqualPropertyKey(key,UI_PKEY_LargeImage),image);
                        return SUCCEEDED(hr)?UIInitPropertyFromImage(key,image.Get(),value):hr;
                    }
                    return E_NOTIMPL;
                }
                id=owner_.applicationId(id);
                if(id&&IsEqualPropertyKey(key,UI_PKEY_ItemsSource)) {
                    auto& read=owner_.collectionReads[id];++read.sourceRequests;read.currentVariantType=current?current->vt:static_cast<UINT>(-1);
                }
                if(!id) {
                    const auto nativeType=owner_.nativeCommandTypes.find(originalId);
                    if(IsEqualPropertyKey(key,UI_PKEY_Enabled))return InitPropVariantFromBoolean(nativeType!=owner_.nativeCommandTypes.end()&&nativeType->second==UI_COMMANDTYPE_GROUP,value);
                    if(IsEqualPropertyKey(key,UI_PKEY_ContextAvailable))return InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_NOTAVAILABLE,value);
                    return E_NOTIMPL;
                }
                const auto type=owner_.commandTypes.find(id);
                const bool action=type!=owner_.commandTypes.end() && type->second!=UI_COMMANDTYPE_GROUP && type->second!=UI_COMMANDTYPE_CONTEXT;
                const auto state=action&&owner_.callbacks.query?owner_.callbacks.query(id):RibbonCommandState{};
                if(owner_.revision->retired)return changedBinding;
                if(IsEqualPropertyKey(key,UI_PKEY_Enabled))return InitPropVariantFromBoolean(state.enabled,value);
                if(IsEqualPropertyKey(key,UI_PKEY_BooleanValue))return InitPropVariantFromBoolean(state.checked,value);
                if(IsEqualPropertyKey(key,UI_PKEY_SelectedItem)) {
                    auto selected=state.selectedIndex;
                    if(type!=owner_.commandTypes.end()&&type->second==UI_COMMANDTYPE_COLLECTION) {
                        const auto rows=owner_.collectionInvocationIndices.find(originalId);
                        if(rows==owner_.collectionInvocationIndices.end()||selected>=rows->second.size())
                            selected=UI_COLLECTION_INVALIDINDEX;
                    }
                    auto& read=owner_.collectionReads[id];++read.selectedRequests;read.lastSelectedIndex=selected;
                    return InitPropVariantFromUInt32(selected,value);
                }
                if(IsEqualPropertyKey(key,UI_PKEY_Label)||IsEqualPropertyKey(key,UI_PKEY_TooltipTitle)){
                    if(owner_.layout==RibbonLayout::InstalledWindows10) {
                        if(originalId==0x2c60&&owner_.englishPresentation)return InitPropVariantFromString(L"Run",value);
                        // Independent installed BML UIA readback proves that
                        // Search Close and both Video group variants have empty
                        // captions; their separately labelled leaves remain.
                        if(originalId==0x2012||originalId==0x2c61||originalId==0x2931||originalId==0x2803||
                           originalId==0x2c20||originalId==0x2c21)
                            return InitPropVariantFromString(L"",value);
                    }
                    if(!state.label.empty())return InitPropVariantFromString(state.label.c_str(),value);
                    const auto found=owner_.metadata.find(id);
                    if(found!=owner_.metadata.end()&&owner_.layout==RibbonLayout::InstalledWindows10&&
                       found->second.labelSource==LabelSource::AuthoredFallback) {
                        const bool title=IsEqualPropertyKey(key,UI_PKEY_Label);
                        auto& observed=title?owner_.observedNativeLabels:owner_.observedNativeTooltips;
                        auto& resources=title?owner_.nativeLabels:owner_.nativeTooltips;
                        if(observed.insert(originalId).second&&current&&current->vt==VT_LPWSTR&&current->pwszVal&&*current->pwszVal) {
                            resources.insert_or_assign(originalId,current->pwszVal);
                            return PropVariantCopy(value,current);
                        }
                        if(const auto resource=resources.find(originalId);resource!=resources.end())
                            return InitPropVariantFromString(resource->second.c_str(),value);
                        // Some installed BML commands supply no public label
                        // resource. Keep the existing authored fallback rather
                        // than blanking a tab/button; its translation is a
                        // separate host-resource requirement.
                    }
                    if(found!=owner_.metadata.end()&&!found->second.label.empty())
                        return InitPropVariantFromString(found->second.label.c_str(),value);
                    return E_NOTIMPL;
                }
                if(IsEqualPropertyKey(key,UI_PKEY_TooltipDescription)){
                    const auto found=owner_.metadata.find(id);
                    return found!=owner_.metadata.end()&&!found->second.description.empty()?InitPropVariantFromString(found->second.description.c_str(),value):E_NOTIMPL;
                }
                if(IsEqualPropertyKey(key,UI_PKEY_SmallImage)||IsEqualPropertyKey(key,UI_PKEY_LargeImage)){
                    ComPtr<IUIImage> image;
                    const auto hr=owner_.image(id,IsEqualPropertyKey(key,UI_PKEY_LargeImage),image);
                    return SUCCEEDED(hr)?UIInitPropertyFromImage(key,image.Get(),value):hr;
                }
                if(IsEqualPropertyKey(key,UI_PKEY_Categories)&&owner_.layout==RibbonLayout::InstalledWindows10) {
                    const auto items=owner_.callbacks.items?owner_.callbacks.items(id):std::vector<RibbonItem>{};
                    if(owner_.revision->retired)return changedBinding;
                    return owner_.replaceCategories(items,current);
                }
                if(IsEqualPropertyKey(key,UI_PKEY_ItemsSource)||IsEqualPropertyKey(key,UI_PKEY_RecentItems)){
                    if(id==RibbonQuickAccess)return S_FALSE;
                    if(id==RibbonFrequentPlaces){
                        const auto items=owner_.callbacks.items?owner_.callbacks.items(id):std::vector<RibbonItem>{};
                        if(owner_.revision->retired)return changedBinding;
                        if(items.size()>64)return E_INVALIDARG;
                        owner_.recentPins.clear();for(const auto& item:items)owner_.recentPins.push_back(item.pinned);
                        std::vector<ComPtr<IUnknown>> values;
                        for(const auto& item:items){ComPtr<IUISimplePropertySet> properties;properties.Attach(new Item(item,true));ComPtr<IUnknown> unknown;properties.As(&unknown);values.push_back(std::move(unknown));}
                        const auto array=SafeArrayCreateVector(VT_UNKNOWN,0,static_cast<ULONG>(values.size()));
                        if(!array)return E_OUTOFMEMORY;
                        for(LONG i=0;i<static_cast<LONG>(values.size());++i){const auto hr=SafeArrayPutElement(array,&i,values[static_cast<std::size_t>(i)].Get());if(FAILED(hr)){SafeArrayDestroy(array);return hr;}}
                        value->vt=VT_ARRAY|VT_UNKNOWN;value->parray=array;
                        if(IsEqualPropertyKey(key,UI_PKEY_ItemsSource))owner_.requestedCollections.insert(id);
                        return S_OK;
                    }
                    if(owner_.layout!=RibbonLayout::InstalledWindows10&&id!=RibbonNewMenu&&id!=RibbonExtractToGallery&&id!=RibbonLayoutGallery && id!=RibbonShareGallery&&id!=RecentSearches&&id!=SearchDateMenu&&id!=SearchKindMenu&&id!=SearchSizeMenu&&id!=RibbonSearchOtherProperties)return E_NOTIMPL;
                    if(!current||current->vt!=VT_UNKNOWN||!current->punkVal)return E_INVALIDARG;
                    ComPtr<IUICollection> collection;auto hr=current->punkVal->QueryInterface(IID_PPV_ARGS(&collection));if(FAILED(hr))return hr;
                    auto items=owner_.collectionItems(id);
                    if(owner_.revision->retired)return changedBinding;
                    if(items.size()>4096)return E_INVALIDARG;
                    hr=owner_.replaceCollection(id,originalId,type!=owner_.commandTypes.end()?type->second:UI_COMMANDTYPE_COLLECTION,items,collection.Get());
                    // The first provider callback already supplies the current
                    // namespace. Mark the source refreshable after publishing it:
                    // invalidating it during first popup construction leaves the
                    // installed native gallery empty despite a populated source.
                    if(SUCCEEDED(hr)&&IsEqualPropertyKey(key,UI_PKEY_ItemsSource))owner_.requestedCollections.insert(id);
                    return hr;
                }
                return E_NOTIMPL;
            }catch(...){return E_FAIL;}
        }
    private:
        std::atomic<ULONG> references_{1};Impl& owner_;
    };
    ComPtr<Handler> handler;
    HRESULT replaceCategories(const std::vector<RibbonItem>& items,const PROPVARIANT* current) {
        if(!current||current->vt!=VT_UNKNOWN||!current->punkVal)return E_NOTIMPL;
        ComPtr<IUICollection> collection;auto hr=current->punkVal->QueryInterface(IID_PPV_ARGS(&collection));if(FAILED(hr))return hr;
        hr=collection->Clear();if(FAILED(hr))return hr;
        std::vector<UINT> categories;
        for(const auto& item:items)if(item.category!=UI_COLLECTION_INVALIDINDEX&&std::find(categories.begin(),categories.end(),item.category)==categories.end())categories.push_back(item.category);
        for(const auto id:categories) {RibbonItem category;category.category=id;
            ComPtr<IUISimplePropertySet> properties;properties.Attach(new Item(std::move(category)));
            hr=collection->Add(properties.Get());if(FAILED(hr))return hr;}
        return S_OK;
    }
    HRESULT replaceCollection(UINT parent,UINT nativeParent,UI_COMMANDTYPE type,const std::vector<RibbonItem>& items,IUICollection* collection) {
        if(!collection)return E_POINTER;if(items.size()>4096)return E_INVALIDARG;
        const bool dynamicCollection=type==UI_COMMANDTYPE_COMMANDCOLLECTION&&
            (layout==RibbonLayout::InstalledWindows10||parent==RibbonExtractToGallery);
        // Clear/Add can notify native listeners. Keep selection and invocation
        // indexes unavailable until the complete replacement has succeeded.
        collectionInvocationIndices[nativeParent].clear();
        auto hr=collection->Clear();if(FAILED(hr))return hr;
        std::vector<UINT> indices;indices.reserve(items.size());
        if(dynamicCollection) {
            for(auto entry=dynamicCommands.begin();entry!=dynamicCommands.end();) {
                if(entry->second.nativeParent!=nativeParent) {++entry;continue;}
                const auto& item=entry->second.item;
                availableDynamicCommands[item.checkable?UI_COMMANDTYPE_BOOLEAN:UI_COMMANDTYPE_ACTION].push_back(entry->first);
                entry=dynamicCommands.erase(entry);
            }
        }
        UINT index=0;
        for(auto item:items) {
            indices.push_back(item.invocationIndex==UI_COLLECTION_INVALIDINDEX?index:item.invocationIndex);
            ComPtr<IUIImage> picture;
            if(!item.image.empty())imageSpec(item.image,false,picture);
            else if(parent==RibbonLayoutGallery||parent==RibbonShareGallery)image(item.command,false,picture);
            if(dynamicCollection) {
                // Native command galleries use 16-bit IDs in either layout.
                // Recycle only the same immutable command type after its
                // previous collection has been cleared, then invalidate
                // every cached property before exposing the replacement.
                const auto itemType=item.checkable?UI_COMMANDTYPE_BOOLEAN:UI_COMMANDTYPE_ACTION;
                auto& available=availableDynamicCommands[itemType];
                UINT id=0;
                if(!available.empty()) {id=available.back();available.pop_back();}
                else {if(nextDynamicCommand>0xfffe)return HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES);id=nextDynamicCommand++;}
                dynamicCommands.insert_or_assign(id,DynamicCommand{parent,item.invocationIndex==UI_COLLECTION_INVALIDINDEX?index:item.invocationIndex,nativeParent,item});
                item.command=id;
            } else if(layout==RibbonLayout::InstalledWindows10) {const auto translated=nativeId(item.command);if(translated)item.command=translated;}
            ComPtr<IUISimplePropertySet> properties;properties.Attach(new Item(std::move(item),false,picture.Get()));
            hr=collection->Add(properties.Get());if(FAILED(hr))return hr;
            if(dynamicCollection) {
                Variant command;ULONG id=0;
                if(SUCCEEDED(properties->GetValue(UI_PKEY_CommandId,&command.value))&&SUCCEEDED(PropVariantToUInt32(command.value,&id)))
                    requestInvalidation(id,UI_INVALIDATIONS_ALLPROPERTIES,nullptr);
            }
            ++index;
        }
        collectionInvocationIndices[nativeParent]=std::move(indices);
        // Clear/Add resets an item gallery's native selection. Restore its
        // current View, Library or Search value after this callback returns.
        // SelectedItem is the gallery's value: the framework queries it for
        // UI_INVALIDATIONS_VALUE, which requires a null property key.
        if(type==UI_COMMANDTYPE_COLLECTION&&(parent==RibbonLayoutGallery||parent==LibraryDefault||
            parent==LibraryOptimize||parent==RibbonLibraryOptimizeMenu||parent==SearchDateMenu||
            parent==SearchKindMenu||parent==SearchSizeMenu))
            return requestInvalidation(nativeParent,UI_INVALIDATIONS_VALUE,nullptr);
        return S_OK;
    }
    HRESULT sameThread()const noexcept {return thread==GetCurrentThreadId()?S_OK:RPC_E_WRONG_THREAD;}
    HRESULT viewStore(ComPtr<IPropertyStore>& store)const {return ribbon?ribbon.As(&store):E_UNEXPECTED;}
    HRESULT setViewValue(REFPROPERTYKEY key,const PROPVARIANT& value){ComPtr<IPropertyStore>store;auto hr=viewStore(store);if(SUCCEEDED(hr))hr=store->SetValue(key,value);if(SUCCEEDED(hr))hr=store->Commit();return hr;}
    HRESULT image(UINT command,bool large,ComPtr<IUIImage>& output){
        const UINT dpi=GetDpiForWindow(window);const UINT pixels=static_cast<UINT>(MulDiv(large?32:16,static_cast<int>(dpi?dpi:96),96));
        const auto cacheKey=std::make_pair(command,pixels);
        const auto cached=imageCache.find(cacheKey);if(cached!=imageCache.end()){output=cached->second;return S_OK;}
        const auto found=metadata.find(command);if(found==metadata.end()||found->second.icon.empty())return E_NOTIMPL;
        const auto hr=imageSpec(found->second.icon,large,output);
        if(SUCCEEDED(hr))imageCache.emplace(cacheKey,output);
        return hr;
    }
    HRESULT imageSpec(const std::wstring& specification,bool large,ComPtr<IUIImage>& output) {
        const UINT dpi=GetDpiForWindow(window);const UINT pixels=static_cast<UINT>(MulDiv(large?32:16,static_cast<int>(dpi?dpi:96),96));
        const auto cacheKey=std::make_pair(specification,pixels);
        if(const auto cached=itemImageCache.find(cacheKey);cached!=itemImageCache.end()) {output=cached->second;return S_OK;}
        if(specification.empty()) {
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=pixels;
            info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* bits=nullptr;const auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!bitmap)return HRESULT_FROM_WIN32(GetLastError()?GetLastError():ERROR_OUTOFMEMORY);
            memset(bits,0,static_cast<size_t>(pixels)*pixels*4);
            const auto result=images->CreateImage(bitmap,UI_OWNERSHIP_TRANSFER,&output);
            if(FAILED(result))DeleteObject(bitmap);else itemImageCache.emplace(cacheKey,output);
            return result;
        }
        if(specification==L"ExplorerFrame.dll:IMAGE:EasyAccess") {
            const auto module=LoadLibraryExW(L"ExplorerFrame.dll",nullptr,
                LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if(!module)return HRESULT_FROM_WIN32(GetLastError());
            struct ResourceModule {HMODULE module;~ResourceModule(){FreeLibrary(module);}} lifetime{module};
            const UINT id=pixels<=16?60045:pixels<=20?60046:pixels<=24?60047:pixels<=32?60048:
                pixels<=40?60050:pixels<=48?60051:60052;
            const auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),L"IMAGE");
            if(!resource)return HRESULT_FROM_WIN32(GetLastError());
            const DWORD bytes=SizeofResource(module,resource);
            const auto loaded=LoadResource(module,resource);
            auto* data=static_cast<BYTE*>(LockResource(loaded));
            if(!bytes||!data)return E_UNEXPECTED;
            auto hr=S_OK;
            if(!imaging)hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging));
            ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICBitmapScaler> scaler;ComPtr<IWICFormatConverter> converter;
            if(SUCCEEDED(hr))hr=imaging->CreateStream(&stream);
            if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(data,bytes);
            if(SUCCEEDED(hr))hr=imaging->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder);
            if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
            if(SUCCEEDED(hr))hr=imaging->CreateBitmapScaler(&scaler);
            if(SUCCEEDED(hr))hr=scaler->Initialize(frame.Get(),pixels,pixels,WICBitmapInterpolationModeFant);
            if(SUCCEEDED(hr))hr=imaging->CreateFormatConverter(&converter);
            if(SUCCEEDED(hr))hr=converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
            if(FAILED(hr))return hr;
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(pixels);
            info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* bits=nullptr;const auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!bitmap)return E_OUTOFMEMORY;
            hr=converter->CopyPixels(nullptr,pixels*4,pixels*pixels*4,static_cast<BYTE*>(bits));
            ComPtr<IUIImage> image;
            if(SUCCEEDED(hr))hr=images->CreateImage(bitmap,UI_OWNERSHIP_TRANSFER,&image);
            if(FAILED(hr)){DeleteObject(bitmap);return hr;}
            itemImageCache.emplace(cacheKey,image);output=std::move(image);return S_OK;
        }
        auto location=specification;const auto comma=location.rfind(L',');if(comma==std::wstring::npos)return E_NOTIMPL;
        wchar_t* end=nullptr;const long iconIndex=wcstol(location.c_str()+comma+1,&end,10);if(!end||*end||iconIndex<std::numeric_limits<int>::min()||iconIndex>std::numeric_limits<int>::max())return E_INVALIDARG;
        location.resize(comma);if(location.size()>1&&location.front()==L'"'&&location.back()==L'"')location=location.substr(1,location.size()-2);
        if(!location.empty()&&location.front()==L'@')location.erase(location.begin());
        std::array<wchar_t,32768> expanded{};const DWORD needed=ExpandEnvironmentStringsW(location.c_str(),expanded.data(),static_cast<DWORD>(expanded.size()));if(!needed||needed>expanded.size())return E_INVALIDARG;
        // Ribbon controls request both sizes of each icon. One extraction
        // returns both from a single module load. Establish the requested
        // result first; caching the other size is optional.
        struct OwnedIcon {HICON value=nullptr;~OwnedIcon(){if(value)DestroyIcon(value);}};
        const UINT pair=static_cast<UINT>(MulDiv(large?16:32,static_cast<int>(dpi?dpi:96),96));
        OwnedIcon largeIcon,smallIcon;
        auto hr=SHDefExtractIconW(expanded.data(),static_cast<int>(iconIndex),0,&largeIcon.value,&smallIcon.value,
            MAKELONG(large?pixels:pair,large?pair:pixels));
        OwnedIcon& requested=large?largeIcon:smallIcon;
        OwnedIcon& paired=large?smallIcon:largeIcon;
        if(FAILED(hr)||!requested.value) {
            OwnedIcon single;
            hr=SHDefExtractIconW(expanded.data(),static_cast<int>(iconIndex),0,&single.value,nullptr,MAKELONG(pixels,0));
            if(FAILED(hr)||!single.value)return FAILED(hr)?hr:E_FAIL;
            std::swap(requested.value,single.value);
            if(paired.value){DestroyIcon(paired.value);paired.value=nullptr;}
        }
        const auto convert=[this](HICON icon,UINT size,ComPtr<IUIImage>& result)->HRESULT {
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(size);info.bmiHeader.biHeight=-static_cast<LONG>(size);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* bits=nullptr;const HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);if(!bitmap)return E_OUTOFMEMORY;
            const HDC dc=CreateCompatibleDC(nullptr);if(!dc){DeleteObject(bitmap);return E_OUTOFMEMORY;}
            const auto old=SelectObject(dc,bitmap);ZeroMemory(bits,static_cast<std::size_t>(size)*size*4);const BOOL drawn=DrawIconEx(dc,0,0,icon,static_cast<int>(size),static_cast<int>(size),0,nullptr,DI_NORMAL);
            SelectObject(dc,old);DeleteDC(dc);
            if(!drawn){DeleteObject(bitmap);return E_FAIL;}
            const auto created=images->CreateImage(bitmap,UI_OWNERSHIP_TRANSFER,&result);if(FAILED(created))DeleteObject(bitmap);
            return created;
        };
        ComPtr<IUIImage> image;
        hr=convert(requested.value,pixels,image);
        if(FAILED(hr))return hr;
        itemImageCache.emplace(cacheKey,image);output=std::move(image);
        // A primary allocation failure still propagates as before. Failure
        // to allocate the optional paired bitmap/cache must not replace a
        // successfully created requested image with an out-of-memory error.
        try {
            ComPtr<IUIImage> pairedImage;
            if(paired.value&&SUCCEEDED(convert(paired.value,pair,pairedImage)))
                itemImageCache.emplace(std::make_pair(specification,pair),pairedImage);
        } catch(const std::bad_alloc&) {}
        return S_OK;
    }
    HRESULT quickCollection(ComPtr<IUICollection>& collection)const {
        Variant value;auto hr=framework->GetUICommandProperty(nativeId(RibbonQuickAccess),UI_PKEY_ItemsSource,&value.value);
        if(FAILED(hr))return hr;
        return value.value.vt==VT_UNKNOWN&&value.value.punkVal?value.value.punkVal->QueryInterface(IID_PPV_ARGS(&collection)):E_UNEXPECTED;
    }
};

std::wstring_view ribbonCommandStoreName(UINT command) noexcept {
    for (const auto& entry : registryCommands) if (entry.command == command) return entry.key;
    return {};
}

NativeRibbon::NativeRibbon():callbackEpoch_(std::make_shared<std::atomic<std::uint64_t>>(0)){}
NativeRibbon::~NativeRibbon(){reset();}
HRESULT NativeRibbon::initialize(HWND window,HINSTANCE instance,RibbonCallbacks callbacks,RibbonLayout layout){
    ++*callbackEpoch_; // Even a failed/new initialization entry revokes an old shutdown commit.
    if(!IsWindow(window)||!instance)return E_INVALIDARG;
    if(impl_)return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    APTTYPE apartment{};APTTYPEQUALIFIER qualifier{};auto hr=CoGetApartmentType(&apartment,&qualifier);
    if(FAILED(hr))return hr;if(apartment!=APTTYPE_STA&&apartment!=APTTYPE_MAINSTA)return RPC_E_WRONG_THREAD;
    try {
        auto impl=std::make_shared<Impl>();impl->window=window;impl->thread=GetCurrentThreadId();impl->callbacks=std::move(callbacks);
        impl->callbackEpoch=callbackEpoch_;
        impl->englishPresentation=englishPresentationLanguage();
        impl->invalidationMessage=RegisterWindowMessageW(L"WindowsExplorer.NativeRibbon.DeferredInvalidations.v1");
        if(!impl->invalidationMessage)return HRESULT_FROM_WIN32(GetLastError());
        if(!SetWindowSubclass(window,Impl::subclassProcedure,reinterpret_cast<UINT_PTR>(impl.get()),reinterpret_cast<DWORD_PTR>(impl.get())))
            return HRESULT_FROM_WIN32(GetLastError()?GetLastError():ERROR_NOT_ENOUGH_MEMORY);
        impl->subclassAttached=true;
        if(layout==RibbonLayout::InstalledWindows10) {
            impl->stockModule=compatibleStockRibbon();
            impl->stockStatus=impl->stockModule?S_OK:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            if(impl->stockModule)impl->layout=RibbonLayout::InstalledWindows10;
        }
        installedRibbonFeatures(&impl->features);
        if(impl->layout==RibbonLayout::InstalledWindows10)impl->updateTemplateAliases();
        for(const auto& command:registryCommands) {
            NamespaceCommandMetadata metadata;
            namespaceCommandMetadata(command.key,&metadata);
            impl->metadata.emplace(command.command,Impl::Metadata{metadata.label,metadata.description,metadata.icon,
                metadata.label.empty()?Impl::LabelSource::AuthoredFallback:Impl::LabelSource::NativeProvider});
        }
        impl->metadata[RibbonShareGallery].icon=impl->metadata[RibbonSpecificPeople].icon;
        for(const auto& [group,command]:groupIcons){const auto found=impl->metadata.find(command);if(found!=impl->metadata.end())impl->metadata.emplace(group,Impl::Metadata{{},{},found->second.icon});}
        for (const auto& compiled : compiledLabels) {
            auto& metadata = impl->metadata[compiled.command];
            if (metadata.label.empty()) metadata.label = compiled.label;
        }
        if(impl->englishPresentation) {
            // These English stock-Ribbon captions intentionally differ from
            // the generic CommandStore title. Preserve that presentation on
            // English systems without replacing another language's native text.
            const auto correction=[&](UINT command,const wchar_t* label) {
                auto& metadata=impl->metadata[command];metadata.label=label;
                metadata.labelSource=Impl::LabelSource::EnglishPresentation;
            };
            correction(RibbonEasyAccessMenu,L"Easy access");
            correction(RibbonOptionsMenu,L"Options");
            correction(RibbonFolderOptions,L"Options");
            correction(RibbonAccessMedia,L"Access media");
            correction(1527,L"Extract To");
        }
        impl->metadata[Delete].label=impl->metadata[RibbonDeleteMenu].label;
        impl->metadata[Delete].labelSource=impl->metadata[RibbonDeleteMenu].labelSource;
        if(impl->metadata[Extract].label.empty())impl->metadata[Extract].label=L"Extract all";
        impl->metadata[RibbonOpenSettings].icon=L"shell32.dll,-16826";
        impl->metadata[RibbonEasyAccessMenu].icon=L"ExplorerFrame.dll:IMAGE:EasyAccess";
        impl->metadata[Delete].icon=impl->metadata[RibbonDeleteMenu].icon;
        // CommandStore's generic pane icons use the reverse pair. Windows 10
        // Explorer's Ribbon uses the plain Preview pane and lined Details pane.
        impl->metadata[PreviewPane].icon=L"shell32.dll,-16757";
        impl->metadata[DetailsPane].icon=L"shell32.dll,-16814";
        // Public Windows.IconSize subcommands use these system icon resources
        // in the documented eight-mode order. No DLL artwork is redistributed.
        constexpr std::array viewIcons{63001,63008,63009,63010,63000,62998,62999,63011};
        std::vector<NamespaceSubcommandMetadata> nativeViews;
        const bool nativeViewMetadata = SUCCEEDED(namespaceCommandChildren(L"Windows.IconSize",nullptr,nullptr,&nativeViews)) && nativeViews.size()==8;
        for(UINT i=0;i<viewIcons.size();++i) {
            const bool nativeTitle=nativeViewMetadata&&!impl->englishPresentation&&!nativeViews[i].label.empty();
            impl->viewTitles[i] = nativeTitle ? nativeViews[i].label : viewLabels[i];
            impl->metadata[ViewFirst+i] = Impl::Metadata{impl->viewTitles[i],{},nativeViewMetadata ? nativeViews[i].icon : L"shell32.dll,-"+std::to_wstring(viewIcons[i]),
                nativeTitle?Impl::LabelSource::NativeProvider:impl->englishPresentation?Impl::LabelSource::EnglishPresentation:Impl::LabelSource::AuthoredFallback};
        }
        for(const auto& [command,metadata]:impl->metadata)impl->defaultIcons.emplace(command,metadata.icon);
        impl->handler.Attach(new Impl::Handler(*impl));
        hr=CoCreateInstance(CLSID_UIRibbonFramework,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&impl->framework));if(FAILED(hr))return hr;
        hr=CoCreateInstance(CLSID_UIRibbonImageFromBitmapFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&impl->images));if(FAILED(hr))return hr;
        hr=impl->framework->Initialize(window,impl->handler.Get());if(FAILED(hr))return hr;
        hr=impl->framework->LoadUI(impl->stockModule?impl->stockModule:instance,impl->stockModule?L"EXPLORER_RIBBON":L"APPLICATION_RIBBON");
        if(impl->stockModule)impl->stockStatus=hr;
        if(FAILED(hr)&&impl->stockModule) {
            impl->framework->Destroy();impl->framework.Reset();impl->ribbon.Reset();
            impl->deferredInvalidations.clear();impl->requestedCollections.clear();impl->dynamicCommands.clear();
            impl->collectionReads.clear();impl->collectionInvocationIndices.clear();impl->recentPins.clear();
            impl->availableDynamicCommands.clear();impl->nextDynamicCommand=0x9000;
            FreeLibrary(impl->stockModule);impl->stockModule=nullptr;impl->layout=RibbonLayout::Authored;
            impl->commandTypes.clear();impl->nativeCommandTypes.clear();
            hr=CoCreateInstance(CLSID_UIRibbonFramework,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&impl->framework));
            if(SUCCEEDED(hr))hr=impl->framework->Initialize(window,impl->handler.Get());
            if(SUCCEEDED(hr))hr=impl->framework->LoadUI(instance,L"APPLICATION_RIBBON");
        }
        if(FAILED(hr)){impl->framework->Destroy();return hr;}
        if(impl->layout==RibbonLayout::InstalledWindows10)impl->publicFramework.Attach(new StockFramework(impl->framework.Get(),impl->translation,impl->revision));
        impl_=std::move(impl);++bindingGeneration_;
        hr=setComputerMode(false);if(FAILED(hr)){reset();return hr;}
        hr=setContexts(RibbonContext::None);if(FAILED(hr)){reset();return hr;}
        return S_OK;
    }catch(...){return E_OUTOFMEMORY;}
}
void NativeRibbon::reset()noexcept{
    const auto epoch=++*callbackEpoch_; // Nested reset without an Impl still revokes the old transaction.
    if(!impl_)return;
    // Retire before any native callback: old QAT/state/commands stay blocked.
    // Retain only a stack-scoped exception for the original RecentItems commit.
    auto retired=std::move(impl_);++bindingGeneration_;++retired->revision->value;retired->revision->retired=true;
    retired->deferredInvalidations.clear();
    const auto framework=retired->framework;
    {
        struct ShutdownCommit {
            Impl& owner;
            ~ShutdownCommit(){
                owner.shutdownRecentItems=false;owner.shutdownRecentItemsFramework=nullptr;
                owner.shutdownPinItem={};owner.shutdownRecentPins.clear();
            }
        } commit{*retired};
        retired->shutdownRecentItemsEpoch=epoch;retired->shutdownRecentItemsFramework=framework.Get();
        try {
            retired->shutdownRecentPins=retired->recentPins;retired->shutdownPinItem=retired->callbacks.pinItem;
            retired->shutdownRecentItemsStarted=false;
            retired->shutdownRecentItems=retired->callbackEpoch&&retired->callbackEpoch->load()==epoch;
        }catch(...) {retired->shutdownRecentItems=false;}
        if(framework)framework->Destroy();
    }
    // Keep the old unique subclass until Destroy returns so WM_NCDESTROY can
    // invalidate the original window even when a callback reuses its HWND.
    // Retirement blocks its invalidations; this ID never detaches a new Impl.
    retired->detachSubclass();
    retired->ribbon.Reset();retired->framework.Reset();retired->handler.Reset();
}
bool NativeRibbon::valid()const noexcept{return impl_&&impl_->framework&&impl_->ribbon;}
UINT NativeRibbon::height()const noexcept{return impl_?impl_->height:0;}
IUIFramework* NativeRibbon::framework()const noexcept{return impl_?(impl_->publicFramework?impl_->publicFramework.Get():impl_->framework.Get()):nullptr;}
IUIFramework* NativeRibbon::nativeFramework()const noexcept{return impl_?impl_->framework.Get():nullptr;}
UINT NativeRibbon::nativeCommandId(UINT command)const noexcept{return impl_?impl_->nativeId(command):0;}
RibbonLayout NativeRibbon::layout()const noexcept{return impl_?impl_->layout:RibbonLayout::Authored;}
HRESULT NativeRibbon::installedLayoutStatus()const noexcept{return impl_?impl_->stockStatus:E_UNEXPECTED;}
HRESULT NativeRibbon::invalidate(UINT command){if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;const auto id=impl_->nativeId(command);return command&&!id?HRESULT_FROM_WIN32(ERROR_NOT_FOUND):impl_->requestInvalidation(id,UI_INVALIDATIONS_ALLPROPERTIES,nullptr);}
HRESULT NativeRibbon::invalidateState(UINT command){if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;const auto id=impl_->nativeId(command);return command&&!id?HRESULT_FROM_WIN32(ERROR_NOT_FOUND):impl_->requestInvalidation(id,UI_INVALIDATIONS_STATE|UI_INVALIDATIONS_VALUE,nullptr);}
HRESULT NativeRibbon::invalidateItems(UINT command) {
    if(!valid())return E_UNEXPECTED;const auto thread=impl_->sameThread();if(FAILED(thread))return thread;
    const auto lifetime=impl_;const auto framework=lifetime->framework;const auto generation=bindingGeneration_;
    const auto current=[&]{return impl_.get()==lifetime.get()&&bindingGeneration_==generation&&impl_->framework.Get()==framework.Get();};
    const auto requested=lifetime->requestedCollections;
    HRESULT result=S_OK;
    for(const auto id:requested)if(!command||command==id) {
        const auto hr=lifetime->requestInvalidation(lifetime->nativeId(id),UI_INVALIDATIONS_PROPERTY,&UI_PKEY_ItemsSource);
        if(!current())return changedBinding;
        lifetime->collectionReads[id].lastInvalidation=hr;
        if(FAILED(hr)&&SUCCEEDED(result))result=hr;
        if(lifetime->layout==RibbonLayout::InstalledWindows10) {
            const auto categories=lifetime->requestInvalidation(lifetime->nativeId(id),UI_INVALIDATIONS_PROPERTY,&UI_PKEY_Categories);
            if(!current())return changedBinding;
            if(FAILED(categories)&&SUCCEEDED(result))result=categories;
        }
    }
    return result;
}
HRESULT NativeRibbon::collectionReadback(UINT command,RibbonCollectionReadback& output) const {
    if(!valid())return E_UNEXPECTED;const auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto found=impl_->collectionReads.find(command);if(found==impl_->collectionReads.end())return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    output=found->second;
    const auto native=impl_->nativeId(command);
    if(const auto rows=impl_->collectionInvocationIndices.find(native);rows!=impl_->collectionInvocationIndices.end())
        output.publishedItems=static_cast<UINT>(rows->second.size());
    output.pendingInvalidations=static_cast<UINT>(std::count_if(impl_->deferredInvalidations.begin(),impl_->deferredInvalidations.end(),
        [native](const auto& entry){return entry.command==native;}));
    return S_OK;
}
HRESULT NativeRibbon::flush(){
    if(!valid())return E_UNEXPECTED;const auto lifetime=impl_;auto hr=lifetime->sameThread();if(FAILED(hr))return hr;
    if(lifetime->propertyDepth)return E_PENDING;const auto framework=lifetime->framework;const auto generation=bindingGeneration_;
    const auto current=[&]{return impl_.get()==lifetime.get()&&bindingGeneration_==generation&&impl_->framework.Get()==framework.Get();};
    hr=lifetime->drainInvalidations();if(!current())return changedBinding;if(FAILED(hr))return hr;
    hr=framework->FlushPendingInvalidations();return current()?hr:changedBinding;
}
HRESULT NativeRibbon::setContexts(RibbonContext contexts,bool activate){
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto flags=static_cast<UINT>(contexts);if(flags&~2047u)return E_INVALIDARG;
    if(impl_->layout==RibbonLayout::InstalledWindows10) {
        for(const auto& [id,type]:impl_->nativeCommandTypes)if(type==UI_COMMANDTYPE_CONTEXT) {
            const auto application=impl_->applicationId(id);
            const bool available=application>=RibbonPictureContext&&application<=RibbonShortcutContext&&
                id==impl_->nativeId(application)&&(flags&(1u<<(application-RibbonPictureContext)));
            Variant value;InitPropVariantFromUInt32(available?(activate?UI_CONTEXTAVAILABILITY_ACTIVE:UI_CONTEXTAVAILABILITY_AVAILABLE):UI_CONTEXTAVAILABILITY_NOTAVAILABLE,&value.value);
            hr=impl_->framework->SetUICommandProperty(id,UI_PKEY_ContextAvailable,value.value);if(FAILED(hr))return hr;
        }
    } else for(UINT i=0;i<11;++i){
        if(!impl_->commandTypes.contains(RibbonPictureContext+i)) {if(flags&(1u<<i))return E_NOTIMPL;continue;}
        Variant value;InitPropVariantFromUInt32((flags&(1u<<i))?(activate?UI_CONTEXTAVAILABILITY_ACTIVE:UI_CONTEXTAVAILABILITY_AVAILABLE):UI_CONTEXTAVAILABILITY_NOTAVAILABLE,&value.value);hr=impl_->framework->SetUICommandProperty(RibbonPictureContext+i,UI_PKEY_ContextAvailable,value.value);if(FAILED(hr))return hr;}
    impl_->contexts=contexts;impl_->activeContext=activate;return S_OK;
}
HRESULT NativeRibbon::setComputerMode(bool computer){if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;hr=impl_->framework->SetModes(impl_->layout==RibbonLayout::InstalledWindows10?(computer?impl_->computerModes():impl_->homeModes()):(computer?2:1));if(SUCCEEDED(hr)){impl_->computerMode=computer;impl_->networkMode=false;}return hr;}
HRESULT NativeRibbon::setNetworkMode(bool network,bool activeDirectory){if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    hr=impl_->framework->SetModes(impl_->layout==RibbonLayout::InstalledWindows10?(network?(activeDirectory?0x202:0x402):impl_->homeModes()):(network?4:1));if(SUCCEEDED(hr)){impl_->networkMode=network;impl_->computerMode=false;}return hr;}
HRESULT NativeRibbon::setDriveType(UINT type) {
    if(!valid())return E_UNEXPECTED;const auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    if(type>DRIVE_RAMDISK)return E_INVALIDARG;if(type==impl_->driveType)return S_FALSE;
    impl_->driveType=type;if(impl_->layout!=RibbonLayout::InstalledWindows10)return S_OK;
    impl_->updateTemplateAliases();return setContexts(impl_->contexts,impl_->activeContext);
}
RibbonFeatures NativeRibbon::features()const noexcept {return impl_?impl_->features:RibbonFeatures{};}
HRESULT NativeRibbon::contextAvailable(RibbonContext context,UINT& identifier,UINT& availability)const {
    if(!valid())return E_UNEXPECTED;const auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto flags=static_cast<UINT>(context);if(!flags||(flags&(flags-1))||flags>1024)return E_INVALIDARG;
    UINT bit=0;while(!(flags&(1u<<bit)))++bit;
    const auto native=impl_->nativeId(RibbonPictureContext+bit);
    Variant property;const auto result=impl_->framework->GetUICommandProperty(native,UI_PKEY_ContextAvailable,&property.value);
    ULONG value=0;const auto converted=SUCCEEDED(result)?PropVariantToUInt32(property.value,&value):result;
    if(SUCCEEDED(converted)){identifier=native;availability=value;}return converted;
}
HRESULT NativeRibbon::selectTab(UINT tab){
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    if((tab>=RibbonPictureTab&&tab<=RibbonDiscImageTab)||tab==RibbonShortcutTab){
        Variant value;InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_ACTIVE,&value.value);
        return framework()->SetUICommandProperty(tab==RibbonShortcutTab?RibbonShortcutContext:RibbonPictureContext+tab-RibbonPictureTab,UI_PKEY_ContextAvailable,value.value);
    }
    const bool applicable=((tab==RibbonHomeTab||tab==RibbonShareTab)&&!impl_->computerMode&&!impl_->networkMode)||
        tab==RibbonViewTab||(tab==RibbonComputerTab&&impl_->computerMode)||(tab==RibbonNetworkTab&&impl_->networkMode);
    if(!applicable)return E_INVALIDARG;
    std::wstring name;hr=commandLabel(tab,name);if(FAILED(hr))return hr;
    // Tab properties are invalidation-only. Its documented accessibility
    // SelectionItem pattern supplies programmatic selection without input
    // injection, activating a desktop, or opening an application command.
    HWND bar=nullptr;
    EnumChildWindows(impl_->window,[](HWND child,LPARAM data)->BOOL {
        wchar_t type[64]{};GetClassNameW(child,type,64);
        if(wcscmp(type,L"UIRibbonCommandBar")==0){*reinterpret_cast<HWND*>(data)=child;return FALSE;}
        return TRUE;
    },reinterpret_cast<LPARAM>(&bar));
    try {return selectTabOnMta(bar?bar:impl_->window,name.c_str());}catch(...){return E_OUTOFMEMORY;}
}
HRESULT NativeRibbon::setViewSetting(REFPROPERTYKEY key,const PROPVARIANT& value) {
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(hr!=S_OK)return hr;
    const auto lifetime=impl_;auto* const original=lifetime.get();const auto framework=original->framework;const auto ribbon=original->ribbon;
    const auto generation=bindingGeneration_;const auto window=original->window;const auto thread=original->thread;
    RibbonMutation mutation(original->revision);const auto revision=mutation.revision->value;
    const auto current=[&]{DWORD process=0;return impl_.get()==original&&bindingGeneration_==generation&&
        impl_->framework.Get()==framework.Get()&&mutation.revision->value==revision&&!mutation.revision->windowDestroyed&&
        IsWindow(window)&&GetWindowThreadProcessId(window,&process)==thread&&process==GetCurrentProcessId();};
    ComPtr<IPropertyStore> store;hr=ribbon.As(&store);if(!current())return changedBinding;
    if(hr!=S_OK||!store)return FAILED(hr)?hr:E_UNEXPECTED;
    hr=store->SetValue(key,value);if(!current())return changedBinding;if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
    hr=store->Commit();if(!current())return changedBinding;return hr;
}
HRESULT NativeRibbon::setMinimized(bool minimized){Variant value;InitPropVariantFromBoolean(minimized,&value.value);return setViewSetting(UI_PKEY_Minimized,value.value);}
HRESULT NativeRibbon::expandSearchDateMenu(std::span<const std::wstring> expectedRows,UINT& matchedRows,
                                         NativePopupCapture* popup) {
    matchedRows=0;if(!valid())return E_UNEXPECTED;
    auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto desktop=PrivateDesktop::current();
    if(!desktop||FAILED(desktop->verifyIsolation()))return E_ACCESSDENIED;
    if(expectedRows.size()!=8||std::any_of(expectedRows.begin(),expectedRows.end(),[](const auto& label){return label.empty();}))return E_INVALIDARG;
    for(size_t index=0;index<expectedRows.size();++index)
        if(std::find(expectedRows.begin(),expectedRows.begin()+static_cast<std::ptrdiff_t>(index),expectedRows[index])!=
            expectedRows.begin()+static_cast<std::ptrdiff_t>(index))return E_INVALIDARG;
    const auto type=impl_->commandTypes.find(SearchDateMenu);
    if(type==impl_->commandTypes.end()||type->second!=UI_COMMANDTYPE_COLLECTION)return E_NOINTERFACE;
    struct RibbonWindows {HWND window=nullptr;UINT count=0;} bars;
    EnumChildWindows(impl_->window,[](HWND child,LPARAM value)->BOOL {
        wchar_t className[64]{};GetClassNameW(child,className,static_cast<int>(std::size(className)));
        if(wcscmp(className,L"UIRibbonCommandBar")==0) {
            auto& found=*reinterpret_cast<RibbonWindows*>(value);found.window=child;++found.count;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&bars));
    if(bars.count!=1||!IsWindowVisible(bars.window))return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if(impl_->readOnlyMenuExpansion)return E_UNEXPECTED;
    std::wstring label;hr=commandLabel(SearchDateMenu,label);if(FAILED(hr))return hr;
    struct ExpansionGuard {
        Impl& owner;
        explicit ExpansionGuard(Impl& value):owner(value){owner.readOnlyMenuExpansion=true;owner.menuExpansionExecutionAttempts=0;}
        ~ExpansionGuard(){owner.readOnlyMenuExpansion=false;}
    } guard(*impl_);
    try {
        NativePopupCapture observed;
        hr=selectTabOnMta(impl_->window,label.c_str(),expectedRows,&matchedRows,&observed,bars.window,
            impl_->nativeId(SearchDateMenu),type->second);
        const auto attempts=impl_->menuExpansionExecutionAttempts;
        observed.executionAttempts=attempts;
        if(popup)*popup=std::move(observed);
        return FAILED(hr)?hr:attempts?E_ACCESSDENIED:hr;
    }catch(...){if(popup)popup->executionAttempts=impl_->menuExpansionExecutionAttempts;return E_OUTOFMEMORY;}
}
HRESULT NativeRibbon::minimized(bool& output)const{if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;ComPtr<IPropertyStore>store;hr=impl_->viewStore(store);if(FAILED(hr))return hr;Variant value;hr=store->GetValue(UI_PKEY_Minimized,&value.value);if(FAILED(hr))return hr;BOOL flag=FALSE;hr=PropVariantToBoolean(value.value,&flag);if(SUCCEEDED(hr))output=flag!=FALSE;return hr;}
HRESULT NativeRibbon::setQuickAccessBelow(bool below){Variant value;InitPropVariantFromUInt32(below?UI_CONTROLDOCK_BOTTOM:UI_CONTROLDOCK_TOP,&value.value);return setViewSetting(UI_PKEY_QuickAccessToolbarDock,value.value);}
HRESULT NativeRibbon::quickAccessBelow(bool& output)const{if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;ComPtr<IPropertyStore>store;hr=impl_->viewStore(store);if(FAILED(hr))return hr;Variant value;hr=store->GetValue(UI_PKEY_QuickAccessToolbarDock,&value.value);ULONG dock=0;if(SUCCEEDED(hr))hr=PropVariantToUInt32(value.value,&dock);if(SUCCEEDED(hr))output=dock==UI_CONTROLDOCK_BOTTOM;return hr;}
HRESULT NativeRibbon::quickAccessSnapshot(RibbonQuickAccessSnapshot& output) const {
    if(!valid())return E_UNEXPECTED;
    auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    try {
        const auto lifetime=impl_;auto* const original=lifetime.get();const auto framework=original->framework;const auto ribbon=original->ribbon;
        const auto generation=bindingGeneration_,revision=original->revision->value;
        const auto revisionState=original->revision;const auto window=original->window;const auto thread=original->thread;
        const auto nativeQuickAccess=original->nativeId(RibbonQuickAccess);
        const auto current=[&]{DWORD process=0;return impl_.get()==original&&bindingGeneration_==generation&&
            impl_->framework.Get()==framework.Get()&&revisionState->value==revision&&!revisionState->windowDestroyed&&
            IsWindow(window)&&GetWindowThreadProcessId(window,&process)==thread&&process==GetCurrentProcessId();};
        RibbonQuickAccessSnapshot captured;captured.impl_=std::make_unique<RibbonQuickAccessSnapshot::Impl>();
        auto& snapshot=*captured.impl_;
        snapshot.owner=this;snapshot.ribbonState=original;snapshot.thread=thread;snapshot.window=window;
        snapshot.generation=generation;snapshot.revision=revision;snapshot.framework=framework;snapshot.nativeQuickAccess=nativeQuickAccess;
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        Variant source;hr=framework->GetUICommandProperty(nativeQuickAccess,UI_PKEY_ItemsSource,&source.value);
        if(!current())return changedBinding;
        if(hr!=S_OK||source.value.vt!=VT_UNKNOWN||!source.value.punkVal)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=source.value.punkVal->QueryInterface(IID_PPV_ARGS(&snapshot.collection));
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK||!snapshot.collection)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=snapshot.collection.As(&snapshot.collectionIdentity);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK||!snapshot.collectionIdentity)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=ribbon.As(&snapshot.viewProperties);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK||!snapshot.viewProperties)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=ribbon.As(&snapshot.viewIdentity);if(!current())return changedBinding;
        if(hr!=S_OK||!snapshot.viewIdentity)return FAILED(hr)?hr:E_UNEXPECTED;
        Variant dock;hr=snapshot.viewProperties->GetValue(UI_PKEY_QuickAccessToolbarDock,&dock.value);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=PropVariantToUInt32(dock.value,&snapshot.dock);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        if(snapshot.dock!=UI_CONTROLDOCK_TOP&&snapshot.dock!=UI_CONTROLDOCK_BOTTOM)return E_UNEXPECTED;
        Variant minimizedValue;hr=snapshot.viewProperties->GetValue(UI_PKEY_Minimized,&minimizedValue.value);
        if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        BOOL minimized=FALSE;hr=PropVariantToBoolean(minimizedValue.value,&minimized);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;snapshot.minimized=minimized!=FALSE;
        UINT count=0;hr=snapshot.collection->GetCount(&count);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        // The public native QAT supports at most20 commands. Do not truncate.
        // https://learn.microsoft.com/windows/win32/windowsribbon/windowsribbon-controls-quickaccesstoolbar
        if(count>20)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        snapshot.rows.reserve(count);snapshot.identities.reserve(count);snapshot.items.reserve(count);snapshot.properties.reserve(count);
        for(UINT index=0;index<count;++index) {
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            ComPtr<IUnknown> row,identity;hr=snapshot.collection->GetItem(index,&row);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(hr!=S_OK||!row)return FAILED(hr)?hr:E_UNEXPECTED;
            hr=row.As(&identity);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(hr!=S_OK||!identity)return FAILED(hr)?hr:E_UNEXPECTED;
            RibbonQuickAccessItem item;item.commandRead=RibbonQuickAccessSnapshot::Impl::readCommand(row.Get(),item.nativeCommand);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(item.commandRead==S_OK)item.command=original->applicationId(item.nativeCommand);
            RibbonQuickAccessSnapshot::Impl::Properties properties;
            hr=RibbonQuickAccessSnapshot::Impl::readProperties(row.Get(),properties,current);if(hr!=S_OK)return hr;
            snapshot.properties.push_back(std::move(properties));
            snapshot.rows.push_back(std::move(row));snapshot.identities.push_back(std::move(identity));snapshot.items.push_back(item);
        }
        hr=snapshot.matches(snapshot.identities,current);if(hr!=S_OK)return hr;
        output=std::move(captured);return S_OK;
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
}

HRESULT NativeRibbon::editQuickAccess(const RibbonQuickAccessSnapshot& captured,const RibbonQuickAccessEdit& edit) {
    if(!valid()||!captured.impl_)return E_UNEXPECTED;
    const auto& snapshot=*captured.impl_;
    if(snapshot.thread!=GetCurrentThreadId())return RPC_E_WRONG_THREAD;
    if(snapshot.owner!=this)return E_INVALIDARG;
    auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto lifetime=impl_;auto* const original=lifetime.get();
    auto revision=original->revision->value;const auto revisionState=original->revision;
    const auto current=[&]{return impl_.get()==original&&snapshot.ribbonState==original&&
        bindingGeneration_==snapshot.generation&&impl_->framework.Get()==snapshot.framework.Get()&&
        revisionState->value==revision&&!revisionState->windowDestroyed&&IsWindow(snapshot.window)&&
        GetWindowThreadProcessId(snapshot.window,nullptr)==snapshot.thread;};
    if(!current()||revision!=snapshot.revision)return HRESULT_FROM_WIN32(ERROR_RETRY);
    try {
        ComPtr<IUICollection> collection;ComPtr<IUnknown> identity;
        hr=original->quickCollection(collection);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK||!collection)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=collection.As(&identity);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK||!identity)return FAILED(hr)?hr:E_UNEXPECTED;
        if(identity.Get()!=snapshot.collectionIdentity.Get())return HRESULT_FROM_WIN32(ERROR_RETRY);
        hr=snapshot.matches(snapshot.identities,current);if(hr!=S_OK)return hr;
        const auto count=static_cast<UINT>(snapshot.rows.size());
        auto expected=snapshot.identities;
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(edit.kind==RibbonQuickAccessEditKind::Add) {
            if(!edit.command||edit.index||edit.destination)return E_INVALIDARG;
            // An unavailable binding cannot prove that the desired command
            // is absent. Preserve opaque rows; do not manufacture a duplicate.
            if(std::any_of(snapshot.items.begin(),snapshot.items.end(),[](const auto& item){
                return item.commandRead!=S_OK;}))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            const auto command=original->nativeId(edit.command);
            if(!command)return E_INVALIDARG;
            const auto declared=std::find_if(std::begin(compiledLabels),std::end(compiledLabels),
                [&](const auto& label){return label.command==edit.command;});
            const auto type=original->commandTypes.find(edit.command);
            if(declared==std::end(compiledLabels)||edit.command==FileMenu||edit.command==RibbonQuickAccess||
               edit.command==RibbonFrequentPlaces||(edit.command>=1500&&edit.command<2000)||
               (edit.command>=RibbonHomeTab&&edit.command<=RibbonShortcutTab)||
               (edit.command>=RibbonPictureContext&&edit.command<=RibbonShortcutContext)||
               (type!=original->commandTypes.end()&&(type->second==UI_COMMANDTYPE_GROUP||type->second==UI_COMMANDTYPE_CONTEXT)))return E_INVALIDARG;
            if(std::any_of(snapshot.items.begin(),snapshot.items.end(),[&](const auto& item){
                return item.commandRead==S_OK&&item.nativeCommand==command;}))return S_FALSE;
            if(count>=20)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_CMDS);
            ComPtr<IUISimplePropertySet> properties;properties.Attach(new Item({command,{},false}));
            ComPtr<IUnknown> row;hr=properties.As(&row);if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
            expected.push_back(row);
            hr=snapshot.matches(snapshot.identities,current);if(hr!=S_OK)return hr;
            RibbonMutation mutation(revisionState);revision=revisionState->value;
            hr=collection->Add(row.Get());
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
            return snapshot.matches(expected,current,command);
        }
        if(edit.command||edit.index>=count||snapshot.items[edit.index].commandRead!=S_OK||
           !snapshot.items[edit.index].command)return E_INVALIDARG;
        if(edit.kind==RibbonQuickAccessEditKind::Remove) {
            if(edit.destination)return E_INVALIDARG;
            expected.erase(expected.begin()+edit.index);
            hr=snapshot.matches(snapshot.identities,current);if(hr!=S_OK)return hr;
            RibbonMutation mutation(revisionState);revision=revisionState->value;
            hr=collection->RemoveAt(edit.index);
            if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
            return snapshot.matches(expected,current);
        }
        if(edit.kind!=RibbonQuickAccessEditKind::Move||edit.destination>=count)return E_INVALIDARG;
        if(edit.index==edit.destination)return S_FALSE;
        const auto moved=snapshot.rows[edit.index];const auto movedIdentity=expected[edit.index];
        expected.erase(expected.begin()+edit.index);
        // Allocate both expected lists before any mutation. A local allocation
        // failure must leave the complete original native collection intact.
        auto reordered=expected;reordered.insert(reordered.begin()+edit.destination,movedIdentity);
        hr=snapshot.matches(snapshot.identities,current);if(hr!=S_OK)return hr;
        RibbonMutation mutation(revisionState);revision=revisionState->value;
        hr=collection->RemoveAt(edit.index);
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=snapshot.matches(expected,current);if(hr!=S_OK)return hr;
        hr=collection->Insert(edit.destination,moved.Get());
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(hr==S_OK)return snapshot.matches(reordered,current);
        const auto operation=FAILED(hr)?hr:E_UNEXPECTED;
        const auto rollbackReady=snapshot.matches(expected,current);
        if(rollbackReady!=S_OK)return rollbackReady;
        const auto rollback=collection->Insert(edit.index,moved.Get());
        if(!current())return HRESULT_FROM_WIN32(ERROR_RETRY);
        if(rollback!=S_OK)return FAILED(rollback)?rollback:E_UNEXPECTED;
        const auto restored=snapshot.matches(snapshot.identities,current);
        return restored==S_OK?operation:restored;
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
}
HRESULT NativeRibbon::quickAccessCommands(std::vector<UINT>& output)const{
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;ComPtr<IUICollection>collection;hr=impl_->quickCollection(collection);if(FAILED(hr))return hr;
    UINT count=0;hr=collection->GetCount(&count);if(FAILED(hr))return hr;if(count>20)return E_UNEXPECTED;
    std::vector<UINT>commands;
    for(UINT i=0;i<count;++i){ComPtr<IUnknown>item;hr=collection->GetItem(i,&item);if(FAILED(hr))return hr;ComPtr<IUISimplePropertySet>properties;hr=item.As(&properties);if(FAILED(hr))return hr;Variant value;hr=properties->GetValue(UI_PKEY_CommandId,&value.value);ULONG id=0;if(SUCCEEDED(hr))hr=PropVariantToUInt32(value.value,&id);if(FAILED(hr))return hr;
        if(impl_->layout==RibbonLayout::InstalledWindows10) {id=impl_->applicationId(id);if(!id)return E_UNEXPECTED;}
        commands.push_back(id);}
    output=std::move(commands);return S_OK;
}
HRESULT NativeRibbon::setQuickAccessCommands(std::span<const UINT> commands){
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(FAILED(hr))return hr;if(commands.size()>20)return E_INVALIDARG;
    const auto lifetime=impl_;auto* const original=lifetime.get();const auto framework=original->framework;const auto revisionState=original->revision;
    const auto generation=bindingGeneration_;auto revision=revisionState->value;
    const auto current=[&]{return impl_.get()==original&&bindingGeneration_==generation&&impl_->framework.Get()==framework.Get()&&
        revisionState->value==revision&&!revisionState->windowDestroyed&&IsWindow(impl_->window)&&
        GetWindowThreadProcessId(impl_->window,nullptr)==impl_->thread;};
    try {
    std::vector<ComPtr<IUnknown>>replacement;
    for(std::size_t i=0;i<commands.size();++i){if(std::find(commands.begin(),commands.begin()+static_cast<std::ptrdiff_t>(i),commands[i])!=commands.begin()+static_cast<std::ptrdiff_t>(i))return E_INVALIDARG;
        if((commands[i]>=RibbonHomeTab&&commands[i]<=RibbonShortcutTab)||commands[i]==FileMenu||commands[i]==RibbonQuickAccess||commands[i]==RibbonFrequentPlaces)return E_INVALIDARG;
        const auto found=impl_->commandTypes.find(commands[i]);
        const auto declared=std::find_if(std::begin(compiledLabels),std::end(compiledLabels),[&](const auto& label){return label.command==commands[i];});
        if(declared==std::end(compiledLabels)||(commands[i]>=1500&&commands[i]<2000)||(commands[i]>=RibbonPictureContext&&commands[i]<=RibbonShortcutContext)||(found!=impl_->commandTypes.end()&&(found->second==UI_COMMANDTYPE_GROUP||found->second==UI_COMMANDTYPE_CONTEXT)))return E_INVALIDARG;
        const auto id=impl_->nativeId(commands[i]);if(!id)return E_INVALIDARG;
        ComPtr<IUISimplePropertySet>properties;properties.Attach(new Item({id,{},false}));ComPtr<IUnknown>unknown;properties.As(&unknown);replacement.push_back(std::move(unknown));}
    RibbonQuickAccessSnapshot previous;hr=quickAccessSnapshot(previous);if(!current()||hr!=S_OK)return changedBinding;
    const auto& before=*previous.impl_;RibbonQuickAccessSnapshot::Impl replacements;
    replacements.rows=replacement;replacements.identities.reserve(replacement.size());replacements.items.reserve(replacement.size());
    replacements.properties.reserve(replacement.size());
    for(const auto& row:replacement) {
        ComPtr<IUnknown> identity;hr=row.As(&identity);if(!current())return changedBinding;if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        RibbonQuickAccessItem item;item.commandRead=RibbonQuickAccessSnapshot::Impl::readCommand(row.Get(),item.nativeCommand);
        if(!current())return changedBinding;if(item.commandRead!=S_OK)return FAILED(item.commandRead)?item.commandRead:E_UNEXPECTED;
        RibbonQuickAccessSnapshot::Impl::Properties properties;hr=RibbonQuickAccessSnapshot::Impl::readProperties(row.Get(),properties,current);
        if(hr!=S_OK)return hr;replacements.identities.push_back(std::move(identity));replacements.items.push_back(item);replacements.properties.push_back(std::move(properties));
    }
    auto expected=before.identities;expected.reserve(20);
    hr=before.matches(expected,current);if(hr!=S_OK)return hr;
    RibbonMutation mutation(revisionState);revision=revisionState->value;
    const auto apply=[&](auto&& operation,std::vector<ComPtr<IUnknown>> next)->HRESULT {
        if(before.matches(expected,current,0,&replacements)!=S_OK)return changedBinding;
        const auto result=operation();if(!current())return changedBinding;
        if(before.matches(next,current,0,&replacements)==S_OK){expected=std::move(next);return result==S_OK?S_OK:FAILED(result)?result:E_UNEXPECTED;}
        if(before.matches(expected,current,0,&replacements)!=S_OK)return changedBinding;
        return result==S_OK?E_UNEXPECTED:FAILED(result)?result:E_UNEXPECTED;
    };
    const auto replace=[&](const std::vector<ComPtr<IUnknown>>& rows,const std::vector<ComPtr<IUnknown>>& identities)->HRESULT {
        while(!expected.empty()) {
            const auto index=static_cast<UINT>(expected.size()-1);auto next=expected;next.pop_back();
            const auto result=apply([&]{return before.collection->RemoveAt(index);},std::move(next));if(result!=S_OK)return result;
        }
        for(std::size_t index=0;index<rows.size();++index) {
            auto next=expected;next.push_back(identities[index]);
            const auto result=apply([&]{return before.collection->Insert(static_cast<UINT>(index),rows[index].Get());},std::move(next));if(result!=S_OK)return result;
        }
        return S_OK;
    };
    try {
        hr=replace(replacement,replacements.identities);if(hr==S_OK)return S_OK;
        if(!current()||before.matches(expected,current,0,&replacements)!=S_OK)return changedBinding;
        const auto restored=replace(before.rows,before.identities);return restored==S_OK?hr:changedBinding;
    }catch(const std::bad_alloc&) {
        if(!current()||before.matches(expected,current,0,&replacements)!=S_OK)return changedBinding;
        try{return replace(before.rows,before.identities)==S_OK?E_OUTOFMEMORY:changedBinding;}catch(...){return changedBinding;}
    }
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
HRESULT NativeRibbon::saveSettings(const std::filesystem::path& path)const{
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(hr!=S_OK)return hr;
    if(path.empty()||!path.is_absolute()||path.native().find(L'\0')!=std::wstring::npos)return E_INVALIDARG;
    try {
        RibbonQuickAccessSnapshot saved;hr=quickAccessSnapshot(saved);if(hr!=S_OK)return hr;
        const auto& before=*saved.impl_;const auto lifetime=impl_;auto* const original=lifetime.get();
        const auto nativeView=original->ribbon;const auto framework=original->framework;const auto revision=original->revision;
        const auto layout=original->layout;
        const auto current=[&]{DWORD process=0;return impl_.get()==original&&bindingGeneration_==before.generation&&
            impl_->framework.Get()==framework.Get()&&impl_->ribbon.Get()==nativeView.Get()&&revision->value==before.revision&&
            !revision->windowDestroyed&&IsWindow(before.window)&&GetWindowThreadProcessId(before.window,&process)==before.thread&&
            process==GetCurrentProcessId();};
        std::vector<UINT> order;order.reserve(before.items.size());
        for(const auto& item:before.items) {
            if(item.commandRead!=S_OK||!item.nativeCommand||std::find(order.begin(),order.end(),item.nativeCommand)!=order.end())
                return invalidSettings;
            order.push_back(item.nativeCommand); // Actual IDs include valid unmapped native commands.
        }
        hr=before.matches(before.identities,current);if(hr!=S_OK)return hr;
        ComPtr<IStream> stream;hr=CreateStreamOnHGlobal(nullptr,TRUE,&stream);if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=nativeView->SaveSettingsToStream(stream.Get());if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=before.matches(before.identities,current);if(hr!=S_OK)return hr;
        std::vector<BYTE> native,envelope;hr=settingsStreamBytes(stream.Get(),native);if(hr!=S_OK)return hr;
        hr=encodeSettings(layout,order,native,envelope);if(hr!=S_OK)return hr;
        hr=before.matches(before.identities,current);if(hr!=S_OK)return hr;
        const auto committed=writeStateFileAtomic(path,envelope);
        if(!current())return changedBinding;
        hr=before.matches(before.identities,current);return hr==S_OK?committed:hr;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
HRESULT NativeRibbon::loadSettings(const std::filesystem::path& path){
    if(!valid())return E_UNEXPECTED;auto hr=impl_->sameThread();if(hr!=S_OK)return hr;
    if(path.empty()||!path.is_absolute()||path.native().find(L'\0')!=std::wstring::npos)return E_INVALIDARG;
    bool nativeMutated=false;
    try {
        // Validate the complete owned wrapper before making any native Load.
        std::vector<BYTE> bytes;hr=readFile(path,bytes);if(hr!=S_OK)return hr;
        SettingsFile settings;hr=decodeSettings(bytes,impl_->layout,settings);if(hr!=S_OK)return hr;
        RibbonQuickAccessSnapshot saved;hr=quickAccessSnapshot(saved);if(hr!=S_OK)return hr;
        const auto& before=*saved.impl_;const auto lifetime=impl_;auto* const original=lifetime.get();
        const auto nativeView=original->ribbon;const auto framework=original->framework;const auto revisionState=original->revision;
        auto revision=revisionState->value;
        const auto current=[&]{DWORD process=0;return impl_.get()==original&&bindingGeneration_==before.generation&&
            impl_->framework.Get()==framework.Get()&&impl_->ribbon.Get()==nativeView.Get()&&revisionState->value==revision&&
            !revisionState->windowDestroyed&&IsWindow(before.window)&&GetWindowThreadProcessId(before.window,&process)==before.thread&&
            process==GetCurrentProcessId();};
        hr=before.matches(before.identities,current);if(hr!=S_OK)return hr;
        ComPtr<IStream> input;input.Attach(SHCreateMemStream(settings.native.data(),static_cast<UINT>(settings.native.size())));
        if(!input)return E_OUTOFMEMORY;
        ComPtr<IStream> previous;hr=CreateStreamOnHGlobal(nullptr,TRUE,&previous);if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=nativeView->SaveSettingsToStream(previous.Get());if(!current())return changedBinding;
        if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
        hr=before.matches(before.identities,current);if(hr!=S_OK)return hr;
        // Reserve the bounded identity manifest before native mutation.
        std::vector<ComPtr<IUnknown>> expected;expected.reserve(20);
        RibbonMutation mutation(revisionState);revision=revisionState->value;
        nativeMutated=true;const auto nativeLoad=nativeView->LoadSettingsFromStream(input.Get());
        if(!current())return changedBinding; // Preserve a newer host/UI edit or retired binding.
        RibbonQuickAccessSnapshot loaded;hr=quickAccessSnapshot(loaded);if(!current()||hr!=S_OK)return changedBinding;
        auto* owned=loaded.impl_.get();expected.assign(owned->identities.begin(),owned->identities.end());
        const RibbonQuickAccessSnapshot::Impl* extraBindings=nullptr;
        // The observed post-native state is the last state attributable to our
        // native call when the binding and scoped revision still agree. Raw
        // borrowed COM edits inside native Load cannot universally be attributed.
        const auto apply=[&](auto&& operation,std::vector<ComPtr<IUnknown>> next)->HRESULT {
            auto ready=owned->matches(expected,current,0,extraBindings);if(ready!=S_OK)return changedBinding;
            const auto result=operation();if(!current())return changedBinding;
            const auto advanced=owned->matches(next,current,0,extraBindings);
            if(advanced==S_OK){expected=std::move(next);return result==S_OK?S_OK:FAILED(result)?result:E_UNEXPECTED;}
            // A failing COM operation may leave either its exact old state or
            // its exact requested state. Anything else has no rollback owner.
            if(owned->matches(expected,current,0,extraBindings)!=S_OK)return changedBinding;
            return result==S_OK?E_UNEXPECTED:FAILED(result)?result:E_UNEXPECTED;
        };
        const auto rollback=[&](HRESULT operation)->HRESULT {
            try {
            if(!current()||owned->matches(expected,current,0,extraBindings)!=S_OK)return changedBinding;
            LARGE_INTEGER zero{};auto restored=previous->Seek(zero,STREAM_SEEK_SET,nullptr);
            if(!current()||restored!=S_OK)return changedBinding;
            restored=nativeView->LoadSettingsFromStream(previous.Get());
            if(!current()||restored!=S_OK)return changedBinding;
            RibbonQuickAccessSnapshot nativeRestored;restored=quickAccessSnapshot(nativeRestored);
            if(!current()||restored!=S_OK)return changedBinding;
            // Native restores state and dock. We restore exact retained rows
            // because another native roundtrip also discards custom order.
            if(nativeRestored.impl_->dock!=before.dock||nativeRestored.impl_->minimized!=before.minimized)return changedBinding;
            loaded=std::move(nativeRestored);owned=loaded.impl_.get();expected=owned->identities;
            extraBindings=&before;
            while(!expected.empty()) {
                auto next=expected;next.pop_back();const auto index=static_cast<UINT>(expected.size()-1);
                restored=apply([&]{return owned->collection->RemoveAt(index);},std::move(next));
                if(restored!=S_OK)return changedBinding;
            }
            for(std::size_t index=0;index<before.rows.size();++index) {
                auto next=expected;next.push_back(before.identities[index]);
                restored=apply([&]{return owned->collection->Insert(static_cast<UINT>(index),before.rows[index].Get());},std::move(next));
                if(restored!=S_OK)return changedBinding;
            }
            return owned->matches(before.identities,current,0,&before)==S_OK?operation:changedBinding;
            }catch(...){return changedBinding;}
        };
        try {
        if(nativeLoad!=S_OK)return rollback(FAILED(nativeLoad)?nativeLoad:E_UNEXPECTED);
        if(!settings.envelope)return S_OK; // Legacy native order is not recoverable from opaque bytes.
        if(owned->items.size()!=settings.order.size())return rollback(invalidSettings);
        std::vector<UINT> actual;actual.reserve(owned->items.size());
        for(const auto& item:owned->items) {
            if(item.commandRead!=S_OK||!item.nativeCommand||std::find(actual.begin(),actual.end(),item.nativeCommand)!=actual.end())return rollback(invalidSettings);
            actual.push_back(item.nativeCommand);
        }
        auto actualSet=actual,desiredSet=settings.order;
        std::sort(actualSet.begin(),actualSet.end());std::sort(desiredSet.begin(),desiredSet.end());
        if(actualSet!=desiredSet)return rollback(invalidSettings);
        hr=owned->matches(expected,current);if(hr!=S_OK)return changedBinding;
        // Move only loaded whole IUnknown rows, including valid unmapped IDs.
        // No native payload parsing, synthetic replacement rows, or state edits.
        for(std::size_t destination=0;destination<settings.order.size();++destination) {
            const auto found=std::find(actual.begin()+static_cast<std::ptrdiff_t>(destination),actual.end(),settings.order[destination]);
            if(found==actual.end())return rollback(invalidSettings);
            const auto index=static_cast<std::size_t>(found-actual.begin());if(index==destination)continue;
            const auto moved=expected[index];
            const auto row=std::find_if(owned->identities.begin(),owned->identities.end(),[&](const auto& identity){return identity.Get()==moved.Get();});
            if(row==owned->identities.end())return rollback(invalidSettings);
            const auto object=owned->rows[static_cast<std::size_t>(row-owned->identities.begin())];
            auto removed=expected;removed.erase(removed.begin()+static_cast<std::ptrdiff_t>(index));
            auto reordered=removed;reordered.insert(reordered.begin()+static_cast<std::ptrdiff_t>(destination),moved);
            hr=apply([&]{return owned->collection->RemoveAt(static_cast<UINT>(index));},std::move(removed));if(hr!=S_OK)return rollback(hr);
            hr=apply([&]{return owned->collection->Insert(static_cast<UINT>(destination),object.Get());},std::move(reordered));if(hr!=S_OK)return rollback(hr);
            const auto id=actual[index];actual.erase(actual.begin()+static_cast<std::ptrdiff_t>(index));actual.insert(actual.begin()+static_cast<std::ptrdiff_t>(destination),id);
        }
        hr=owned->matches(expected,current);return hr==S_OK?S_OK:changedBinding;
        }catch(const std::bad_alloc&){return rollback(E_OUTOFMEMORY);}
    }catch(const std::bad_alloc&){return nativeMutated?changedBinding:E_OUTOFMEMORY;}
}
HRESULT NativeRibbon::commandImage(UINT command,bool large,IUIImage** output){
    if(!output)return E_POINTER;*output=nullptr;if(!valid())return E_UNEXPECTED;
    auto hr=impl_->sameThread();if(FAILED(hr))return hr;ComPtr<IUIImage>image;hr=impl_->image(command,large,image);if(SUCCEEDED(hr))*output=image.Detach();return hr;
}
HRESULT NativeRibbon::commandLabel(UINT command,std::wstring& output)const{
    if(!valid())return E_UNEXPECTED;const auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto found=impl_->metadata.find(command);if(found==impl_->metadata.end()||found->second.label.empty())return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if(impl_->layout==RibbonLayout::InstalledWindows10&&found->second.labelSource==Impl::LabelSource::AuthoredFallback) {
        // This is an owned copy of the actual native currentValue, not a
        // framework getter or a callback reentry. A missing native resource
        // keeps its explicitly authored fallback and its original provenance.
        if(const auto resource=impl_->nativeLabels.find(impl_->nativeId(command));resource!=impl_->nativeLabels.end()) {
            output=resource->second;return S_OK;
        }
    }
    output=found->second.label;return S_OK;
}
HRESULT NativeRibbon::itemImage(const std::wstring& specification,bool large,IUIImage** output) {
    if(!output)return E_POINTER;*output=nullptr;if(!valid())return E_UNEXPECTED;
    const auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    if(specification.empty()||specification.size()>32768||specification.find(L'\0')!=std::wstring::npos)return E_INVALIDARG;
    ComPtr<IUIImage> image;const auto result=impl_->imageSpec(specification,large,image);
    if(SUCCEEDED(result))*output=image.Detach();return result;
}
HRESULT NativeRibbon::setCommandImageSpec(UINT command,const std::wstring& specification) {
    if(!valid())return E_UNEXPECTED;
    const auto hr=impl_->sameThread();if(FAILED(hr))return hr;
    const auto found=impl_->metadata.find(command);
    if(found==impl_->metadata.end()||specification.size()>32768||specification.find(L'\0')!=std::wstring::npos)return E_INVALIDARG;
    const auto& icon=specification.empty()?impl_->defaultIcons.at(command):specification;
    if(found->second.icon==icon)return S_FALSE;
    found->second.icon=icon;
    for(auto item=impl_->imageCache.begin();item!=impl_->imageCache.end();) {
        if(item->first.first==command)item=impl_->imageCache.erase(item);else ++item;
    }
    if(impl_->commandTypes.contains(command)) {
        auto result=impl_->requestInvalidation(impl_->nativeId(command),UI_INVALIDATIONS_PROPERTY,&UI_PKEY_SmallImage);
        if(SUCCEEDED(result))result=impl_->requestInvalidation(impl_->nativeId(command),UI_INVALIDATIONS_PROPERTY,&UI_PKEY_LargeImage);
        return result;
    }
    return S_OK;
}
}
