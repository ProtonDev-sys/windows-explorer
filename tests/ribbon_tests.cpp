#include "explorer/ribbon.hpp"
#include "explorer/commands.hpp"
#include "explorer/headless_visual.hpp"
#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <uiautomation.h>
#include <future>
#include <chrono>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void succeeded(HRESULT result,const char* message){if(FAILED(result))throw std::runtime_error(std::string(message)+" HRESULT="+std::to_string(static_cast<ULONG>(result)));}
void pump(){const auto end=GetTickCount64()+150;do{MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}while(GetTickCount64()<end);}
struct Window{HWND handle=nullptr;~Window(){if(handle)DestroyWindow(handle);}};
struct Variant{PROPVARIANT value{};~Variant(){PropVariantClear(&value);}};
HRESULT ownedAutomation(HWND window,std::function<HRESULT(IUIAutomation*)> action) {
    DWORD process=0;
    if(!window||GetWindowThreadProcessId(window,&process)!=GetCurrentThreadId()||process!=GetCurrentProcessId())return E_INVALIDARG;
    const auto desktop=GetThreadDesktop(GetCurrentThreadId());
    auto promise=std::make_shared<std::promise<HRESULT>>();auto result=promise->get_future();
    std::thread worker([desktop,promise,action=std::move(action)] {
        if(!SetThreadDesktop(desktop)){promise->set_value(HRESULT_FROM_WIN32(GetLastError()));return;}
        const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(initialized)){promise->set_value(initialized);return;}
        HRESULT hr=E_FAIL;
        {
            Microsoft::WRL::ComPtr<IUIAutomation2> automation;
            hr=CoCreateInstance(CLSID_CUIAutomation8,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
            if(SUCCEEDED(hr)) {
                automation->put_AutoSetFocus(FALSE);automation->put_ConnectionTimeout(1000);automation->put_TransactionTimeout(2000);
                hr=action(automation.Get());
            }
        }
        CoUninitialize();promise->set_value(hr);
    });
    const auto deadline=GetTickCount64()+6000;
    while(result.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready&&GetTickCount64()<deadline)pump();
    if(result.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){worker.detach();return HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
    worker.join();return result.get();
}
HRESULT namedElement(IUIAutomation* automation,HWND window,const wchar_t* name,IUIAutomationElement** output) {
    if(!output)return E_POINTER;*output=nullptr;
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
    auto hr=automation->ElementFromHandle(window,&root);
    VARIANT caption{};caption.vt=VT_BSTR;caption.bstrVal=SysAllocString(name);
    if(SUCCEEDED(hr))hr=automation->CreatePropertyCondition(UIA_NamePropertyId,caption,&condition);VariantClear(&caption);
    if(SUCCEEDED(hr))hr=root->FindFirst(TreeScope_Descendants,condition.Get(),output);
    return SUCCEEDED(hr)&&!*output?HRESULT_FROM_WIN32(ERROR_NOT_FOUND):hr;
}
HRESULT namedGallery(IUIAutomation* automation,HWND window,const wchar_t* name,IUIAutomationElement** output) {
    if(!output)return E_POINTER;*output=nullptr;
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> captionCondition,buttonCondition,splitCondition,types,condition;
    auto hr=automation->ElementFromHandle(window,&root);
    VARIANT caption{};caption.vt=VT_BSTR;caption.bstrVal=SysAllocString(name);
    if(SUCCEEDED(hr))hr=automation->CreatePropertyCondition(UIA_NamePropertyId,caption,&captionCondition);VariantClear(&caption);
    VARIANT type{};type.vt=VT_I4;type.lVal=UIA_ButtonControlTypeId;
    if(SUCCEEDED(hr))hr=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&buttonCondition);
    type.lVal=UIA_SplitButtonControlTypeId;
    if(SUCCEEDED(hr))hr=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&splitCondition);
    if(SUCCEEDED(hr))hr=automation->CreateOrCondition(buttonCondition.Get(),splitCondition.Get(),&types);
    if(SUCCEEDED(hr))hr=automation->CreateAndCondition(captionCondition.Get(),types.Get(),&condition);
    if(SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> matches;
        hr=root->FindAll(TreeScope_Descendants,condition.Get(),&matches);
        int count=0;if(SUCCEEDED(hr))hr=matches->get_Length(&count);
        LONG width=LONG_MAX;
        for(int index=0;SUCCEEDED(hr)&&index<count;++index) {
            Microsoft::WRL::ComPtr<IUIAutomationElement> candidate;RECT bounds{};
            if(FAILED(matches->GetElement(index,&candidate))||FAILED(candidate->get_CurrentBoundingRectangle(&bounds)))continue;
            // Same-name stock primary/arrow providers both expose Expand.
            // The primary executes the default action; choose the narrow arrow.
            const auto candidateWidth=bounds.right-bounds.left;
            if(std::wstring_view(name)==L"Open") {
                Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker;
                Microsoft::WRL::ComPtr<IUIAutomationElement> parent;
                RECT parentBounds{};CONTROLTYPEID parentType=0;BSTR parentName=nullptr;
                const auto parentRead=automation->get_ControlViewWalker(&walker);
                const auto validParent=SUCCEEDED(parentRead)&&walker&&SUCCEEDED(walker->GetParentElement(candidate.Get(),&parent))&&parent&&
                    SUCCEEDED(parent->get_CurrentControlType(&parentType))&&parentType==UIA_GroupControlTypeId&&
                    SUCCEEDED(parent->get_CurrentBoundingRectangle(&parentBounds))&&SUCCEEDED(parent->get_CurrentName(&parentName));
                const auto matchingParent=validParent&&parentName&&std::wstring_view(parentName)==name;
                SysFreeString(parentName);
                const auto maximumArrowWidth=MulDiv(24,GetDpiForWindow(window),96);
                if(!matchingParent||candidateWidth>maximumArrowWidth||bounds.right!=parentBounds.right||bounds.left<=parentBounds.left||
                   bounds.top<parentBounds.top||bounds.bottom>parentBounds.bottom)continue;
            }
            if(candidateWidth>0&&candidateWidth<width){width=candidateWidth;if(*output)(*output)->Release();*output=candidate.Detach();}
        }
    }
    return SUCCEEDED(hr)&&!*output?HRESULT_FROM_WIN32(ERROR_NOT_FOUND):hr;
}
HRESULT expandOwnedGallery(HWND window,const wchar_t* name) {
    return ownedAutomation(window,[window,label=std::wstring(name)](IUIAutomation* automation) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> button;
        auto hr=namedGallery(automation,window,label.c_str(),&button);
        Microsoft::WRL::ComPtr<IUIAutomationExpandCollapsePattern> expansion;
        if(SUCCEEDED(hr))hr=button->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,IID_PPV_ARGS(&expansion));
        if(SUCCEEDED(hr)&&!expansion)return E_NOINTERFACE;
        return SUCCEEDED(hr)?expansion->Expand():hr;
    });
}
HRESULT galleryExpansionState(HWND window,const wchar_t* name,ExpandCollapseState& state,bool collapse=false) {
    auto result=std::make_shared<ExpandCollapseState>(ExpandCollapseState_LeafNode);
    const auto hr=ownedAutomation(window,[window,label=std::wstring(name),result,collapse](IUIAutomation* automation) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> button;
        auto status=namedGallery(automation,window,label.c_str(),&button);
        Microsoft::WRL::ComPtr<IUIAutomationExpandCollapsePattern> pattern;
        if(SUCCEEDED(status))status=button->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,IID_PPV_ARGS(&pattern));
        if(SUCCEEDED(status)&&!pattern)return E_NOINTERFACE;
        if(SUCCEEDED(status)&&collapse)status=pattern->Collapse();
        if(FAILED(status))return status;
        const auto deadline=GetTickCount64()+2000;
        do {
            status=pattern->get_CurrentExpandCollapseState(result.get());
            if(FAILED(status)||!collapse||*result==ExpandCollapseState_Collapsed)return status;
            // Collapse can finish asynchronously. The owning STA continues
            // pumping in ownedAutomation while this windowless MTA observes
            // the same native pattern; invoke Collapse only once.
            Sleep(10);
        }while(GetTickCount64()<deadline);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    });
    if(SUCCEEDED(hr))state=*result;
    return hr;
}
HRESULT ownedControlHidden(HWND window,const std::wstring& label) {
    return ownedAutomation(window,[window,label](IUIAutomation* automation) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> control;
        const auto hr=namedElement(automation,window,label.c_str(),&control);
        if(hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))return S_OK;
        if(FAILED(hr))return hr;
        BOOL offscreen=FALSE;const auto result=control->get_CurrentIsOffscreen(&offscreen);
        return FAILED(result)?result:offscreen?S_OK:E_UNEXPECTED;
    });
}
HRESULT checkOwnedGalleryRow(HWND window,const wchar_t* name,bool enabled,bool invoke) {
    return ownedAutomation(window,[window,label=std::wstring(name),enabled,invoke](IUIAutomation* automation) {
        std::vector<HWND> windows;
        EnumThreadWindows(GetWindowThreadProcessId(window,nullptr),[](HWND candidate,LPARAM context)->BOOL {
            DWORD process=0;GetWindowThreadProcessId(candidate,&process);
            if(process==GetCurrentProcessId()&&IsWindowVisible(candidate))reinterpret_cast<std::vector<HWND>*>(context)->push_back(candidate);
            return TRUE;
        },reinterpret_cast<LPARAM>(&windows));
        for(const auto candidate:windows) {
            Microsoft::WRL::ComPtr<IUIAutomationElement> row;
            if(FAILED(namedElement(automation,candidate,label.c_str(),&row)))continue;
            BOOL actual=FALSE;auto hr=row->get_CurrentIsEnabled(&actual);if(FAILED(hr))return hr;
            if((actual!=FALSE)!=enabled)return E_UNEXPECTED;
            BOOL offscreen=TRUE;RECT bounds{};CONTROLTYPEID type=0;
            hr=row->get_CurrentIsOffscreen(&offscreen);
            if(SUCCEEDED(hr))hr=row->get_CurrentBoundingRectangle(&bounds);
            if(SUCCEEDED(hr))hr=row->get_CurrentControlType(&type);
            if(FAILED(hr))return hr;
            std::cout<<"Native owned gallery row: type="<<type<<" offscreen="<<offscreen<<" width="<<bounds.right-bounds.left<<" height="<<bounds.bottom-bounds.top<<'\n';
            if(offscreen||bounds.right<=bounds.left||bounds.bottom<=bounds.top)return E_UNEXPECTED;
            if(!invoke)return S_OK;
            Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> command;
            hr=row->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&command));
            return SUCCEEDED(hr)?command->Invoke():hr;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    });
}
}
int main(int argc,char** argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    if(FAILED(desktop.initialize()))return 2;
    const auto initialized=OleInitialize(nullptr);if(FAILED(initialized))return 3;
    int result=0;
    try{
        bool isolated=false;succeeded(desktop.verifyIsolation(&isolated),"Isolation readback");require(isolated,"Input desktop changed");
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};require(InitCommonControlsEx(&controls)!=FALSE,"Common controls");
        WNDCLASSW hostClass{};hostClass.lpfnWndProc=DefWindowProcW;hostClass.hInstance=GetModuleHandleW(nullptr);hostClass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);hostClass.lpszClassName=L"WindowsExplorerHiddenRibbonTest";
        require(RegisterClassW(&hostClass)!=0,"Register hidden Ribbon host class");
        Window window{CreateWindowExW(0,hostClass.lpszClassName,L"Hidden Windows 10 Ribbon",WS_OVERLAPPEDWINDOW,0,0,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)};
        require(window.handle!=nullptr,"Hidden host HWND");
        explorer::NativeRibbon ribbon;
        require(ribbon.initialize(nullptr,GetModuleHandleW(nullptr),{})==E_INVALIDARG,"Invalid host accepted");
        bool copyEnabled=false,destinationEnabled=true,copySourceReady=false,callbackChangePending=false;
        HRESULT callbackInvalidation=E_UNEXPECTED,callbackFlush=S_OK;
        unsigned executions=0;UINT lastCommand=0;unsigned heightEvents=0;unsigned copyItemsQueries=0;unsigned itemExecutions=0;UINT lastItemCommand=0,lastItem=0;
        explorer::RibbonCallbacks callbacks;
        callbacks.execute=[&](UINT id){++executions;lastCommand=id;return S_OK;};
        callbacks.executeItem=[&](UINT id,UINT index){++itemExecutions;lastItemCommand=id;lastItem=index;return S_OK;};
        callbacks.query=[&](UINT id){explorer::RibbonCommandState state;state.enabled=id==explorer::Paste?false:id!=explorer::Copy||copyEnabled;state.checked=id==explorer::HiddenItems;state.selectedIndex=id==explorer::RibbonLayoutGallery?5:UI_COLLECTION_INVALIDINDEX;return state;};
        callbacks.items=[&](UINT id){
            std::vector<explorer::RibbonItem> items;
            if(id==explorer::RibbonExtractToGallery)for(UINT i=0;i<54;++i)
                items.push_back({i,L"Folder "+std::to_wstring(i),false,L"imageres.dll,-3",L"Owned mocked destination",destinationEnabled});
            if(id==explorer::RibbonCopyMenu) {
                ++copyItemsQueries;
                if(!copySourceReady)return items;
                if(callbackChangePending) {
                    callbackChangePending=false;copyEnabled=true;
                    callbackInvalidation=ribbon.invalidateState(explorer::Copy);
                    callbackFlush=ribbon.flush();
                }
                explorer::RibbonItem cascade{0,L"Owned destination group",false,L"imageres.dll,-3"};
                explorer::RibbonItem leaf{0,L"Owned nested destination",false,L"imageres.dll,-3"};
                leaf.invocationIndex=41;cascade.children.push_back(std::move(leaf));
                cascade.invocationIndex=17;
                items.push_back(std::move(cascade));
                items.push_back({1,L"Owned disabled destination",false,{}, {},false});
            }
            if(id==explorer::RibbonOpenWith) {
                items.push_back({0,L"Owned application one",false,L"imageres.dll,-3"});
                items.push_back({1,L"Owned application two",false,L"imageres.dll,-3"});
            }
            return items;
        };
        callbacks.heightChanged=[&](UINT){++heightEvents;};
        const bool stock=argc>1&&std::string_view(argv[1])=="--installed";
        succeeded(ribbon.initialize(window.handle,GetModuleHandleW(nullptr),std::move(callbacks),stock?explorer::RibbonLayout::InstalledWindows10:explorer::RibbonLayout::Authored),"Native framework initialization");
        if(stock&&ribbon.layout()!=explorer::RibbonLayout::InstalledWindows10) {
            std::cout<<"SKIP: installed Windows 10 build19045 layout is unavailable, HRESULT="<<static_cast<ULONG>(ribbon.installedLayoutStatus())<<'\n';
            ribbon.reset();DestroyWindow(window.handle);window.handle=nullptr;OleUninitialize();return 77;
        }
        require(ribbon.valid()&&ribbon.height()>20&&heightEvents>0,"Native Ribbon view/height callback");
        require(!IsWindowVisible(window.handle),"Host was shown");
        // Render solely on the non-input private desktop. This cannot surface
        // a window on the user's desktop and supplies normal native paint/layout.
        ShowWindow(window.handle,SW_SHOWNOACTIVATE);UpdateWindow(window.handle);pump();
        succeeded(ribbon.invalidateState(),"All state properties preserve compiled labels");succeeded(ribbon.flush(),"Flush state labels");pump();
        std::wstring tabLabel;succeeded(ribbon.commandLabel(explorer::RibbonHomeTab,tabLabel),"Compiled Home label survives state invalidation");require(tabLabel==L"Home","Home label disappeared");
        const auto capturePath=std::filesystem::current_path()/L"artifacts"/(L"ribbon-native-home-"+std::to_wstring(GetCurrentProcessId())+L".png");
        std::filesystem::create_directories(capturePath.parent_path());
        MoveWindow(window.handle,0,0,749,510,TRUE);pump();
        explorer::VisualCaptureReport capture;explorer::VisualCaptureOptions captureOptions;
        captureOptions.trimInvisibleFrame=true;
        copyEnabled=true;succeeded(ribbon.invalidate(explorer::Copy),"Capture selected-folder Copy state");succeeded(ribbon.flush(),"Capture state flush");pump();
        succeeded(explorer::captureWindowPng(desktop,window.handle,capturePath,captureOptions,capture),"Private native Home Ribbon snapshot");
        auto captureReport=capturePath;captureReport.replace_extension(L".json");succeeded(explorer::writeVisualCaptureReport(captureReport,capture),"Private native Home inventory");
        copyEnabled=false;succeeded(ribbon.invalidate(explorer::Copy),"Restore copy state");succeeded(ribbon.flush(),"Restore copy state flush");
        MoveWindow(window.handle,0,0,1000,700,TRUE);pump();
        require(ribbon.initialize(window.handle,GetModuleHandleW(nullptr),{})==HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED),"Double initialization accepted");
        succeeded(ribbon.flush(),"Native invalidation flush");pump();
        if(stock) {
            const auto features=ribbon.features();
            require(features.editionStatus!=E_PENDING&&features.mediaFoundationStatus!=E_PENDING&&features.discBurningStatus!=E_PENDING&&features.diskCleanupStatus!=E_PENDING,"Native feature provenance was not initialized");
            const bool noBitLocker=features.editionStatus==S_OK&&!features.bitLocker;
            const auto verifyContext=[&](explorer::RibbonContext context,UINT expected) {
                succeeded(ribbon.setContexts(context,true),"Select actual compiled native feature variant");pump();
                UINT identifier=0,availability=0;
                succeeded(ribbon.contextAvailable(context,identifier,availability),"Compiled variant native context readback");
                require(identifier==expected&&availability==UI_CONTEXTAVAILABILITY_ACTIVE,"Native variant context selection differs from actual feature provenance");
            };
            succeeded(ribbon.setDriveType(DRIVE_FIXED),"Fixed-drive native template");
            verifyContext(explorer::RibbonContext::Drive,noBitLocker?(features.discBurning?(features.diskCleanup?0x707:0x70e):0x709):(features.diskCleanup?0x703:0x708));
            if(noBitLocker) {
                std::wstring caption;succeeded(ribbon.commandLabel(explorer::RibbonBitLocker,caption),"Actual BitLocker presentation label");
                succeeded(ownedControlHidden(window.handle,caption),"Home edition native template omits BitLocker control");
            }
            succeeded(ribbon.setDriveType(DRIVE_REMOVABLE),"Non-fixed drive native template");
            verifyContext(explorer::RibbonContext::Drive,noBitLocker?(features.discBurning?0x70e:0x709):0x708);
            std::wstring cleanup;succeeded(ribbon.commandLabel(explorer::RibbonDiskCleanup,cleanup),"Actual Cleanup presentation label");
            succeeded(ownedControlHidden(window.handle,cleanup),"Non-fixed drive native template omits Cleanup control");
            verifyContext(explorer::RibbonContext::Picture,features.mediaFoundation?0x700:0x70c);
            verifyContext(explorer::RibbonContext::Music,features.mediaFoundation?0x701:0x70b);
            verifyContext(explorer::RibbonContext::Video,features.mediaFoundation?0x702:0x70a);
            verifyContext(explorer::RibbonContext::DiscImage,features.discBurning?0x704:0x70d);
            UINT unchangedId=77,unchangedAvailability=88;
            require(ribbon.contextAvailable(explorer::RibbonContext::Drive|explorer::RibbonContext::Music,unchangedId,unchangedAvailability)==E_INVALIDARG&&unchangedId==77&&unchangedAvailability==88,"Invalid context query changed output");
            require(ribbon.setDriveType(DRIVE_RAMDISK+1)==E_INVALIDARG,"Invalid drive type accepted");
            succeeded(ribbon.setDriveType(DRIVE_UNKNOWN),"Restore actual native drive template policy");
            succeeded(ribbon.setContexts(explorer::RibbonContext::Compressed,true),"Realize native Extract-to command gallery");
            succeeded(ribbon.flush(),"Extract-to native gallery flush");pump();
            Variant extractSource;succeeded(ribbon.framework()->GetUICommandProperty(explorer::RibbonExtractToGallery,UI_PKEY_ItemsSource,&extractSource.value),"Installed native Extract-to source");
            Microsoft::WRL::ComPtr<IUICollection> extractCommands;succeeded(extractSource.value.punkVal->QueryInterface(IID_PPV_ARGS(&extractCommands)),"Native Extract-to command collection");
            UINT extractCount=0;succeeded(extractCommands->GetCount(&extractCount),"Native destination count");require(extractCount==54,"Native destinations were lost");
            Microsoft::WRL::ComPtr<IUnknown> extractRaw;Microsoft::WRL::ComPtr<IUISimplePropertySet> extractItem;
            succeeded(extractCommands->GetItem(0,&extractRaw),"Native destination command");succeeded(extractRaw.As(&extractItem),"Native destination properties");
            Variant extractId;succeeded(extractItem->GetValue(UI_PKEY_CommandId,&extractId.value),"Native destination identity");ULONG destination=0;succeeded(PropVariantToUInt32(extractId.value,&destination),"Native destination identity type");
            Variant destinationState;succeeded(ribbon.framework()->GetUICommandProperty(destination,UI_PKEY_Enabled,&destinationState.value),"Runtime destination command registration");
            for(unsigned generation=0;generation<12;++generation) {
                destinationEnabled=(generation&1)!=0;
                succeeded(ribbon.invalidateItems(explorer::RibbonExtractToGallery),"Refresh retained native destination source generation");
                succeeded(ribbon.flush(),"Commit recycled native destination commands");pump();
                Microsoft::WRL::ComPtr<IUnknown> refreshed;Microsoft::WRL::ComPtr<IUISimplePropertySet> properties;
                succeeded(extractCommands->GetItem(0,&refreshed),"Recycled destination identity");succeeded(refreshed.As(&properties),"Recycled destination properties");
                Variant id;succeeded(properties->GetValue(UI_PKEY_CommandId,&id.value),"Recycled native command ID");ULONG command=0;succeeded(PropVariantToUInt32(id.value,&command),"Recycled ID type");
                require(command>=0x9000&&command<0x9036,"A refreshed destination leaked its native command-ID allocation");
                Variant enabled;succeeded(ribbon.framework()->GetUICommandProperty(command,UI_PKEY_Enabled,&enabled.value),"Recycled destination enabled state");
                BOOL actual=FALSE;succeeded(PropVariantToBoolean(enabled.value,&actual),"Recycled enabled type");require((actual!=FALSE)==destinationEnabled,"Recycled command retained stale provider state");
            }
            destinationEnabled=true;
            succeeded(ribbon.setContexts(explorer::RibbonContext::None),"Destination gallery cleanup");succeeded(ribbon.selectTab(explorer::RibbonHomeTab),"Restore Home command gallery tab");pump();
            SetActiveWindow(window.handle);
            explorer::RibbonCollectionReadback openRead;
            succeeded(ribbon.collectionReadback(explorer::RibbonOpenWith,openRead),"Native Open-with handler registration");
            succeeded(ribbon.invalidateItems(explorer::RibbonOpenWith),"Open-with registered item source refresh");succeeded(ribbon.flush(),"Open-with typed source flush");pump();
            succeeded(ribbon.collectionReadback(explorer::RibbonOpenWith,openRead),"Native Open-with demand diagnostics");
            const auto requestsBeforeExpand=openRead.sourceRequests;
            succeeded(expandOwnedGallery(window.handle,L"Open"),"Actual native Open-with dropdown without executing Open");pump();
            Variant openSource;const auto openSourceResult=ribbon.framework()->GetUICommandProperty(explorer::RibbonOpenWith,UI_PKEY_ItemsSource,&openSource.value);
            UINT openCount=0;Microsoft::WRL::ComPtr<IUICollection> openItems;
            if(SUCCEEDED(openSourceResult)&&openSource.value.vt==VT_UNKNOWN&&openSource.value.punkVal&&SUCCEEDED(openSource.value.punkVal->QueryInterface(IID_PPV_ARGS(&openItems))))openItems->GetCount(&openCount);
            succeeded(ribbon.collectionReadback(explorer::RibbonOpenWith,openRead),"Native Open-with actual source demand");
            require(SUCCEEDED(openSourceResult)&&openCount==2&&openRead.sourceRequests>requestsBeforeExpand&&openRead.currentVariantType==VT_UNKNOWN,"Native dropdown did not request its genuine application collection");
            succeeded(checkOwnedGalleryRow(window.handle,L"Owned application one",true,false),"Actual native Open-with first row");
            succeeded(checkOwnedGalleryRow(window.handle,L"Owned application two",true,false),"Actual native Open-with second row");
            ExpandCollapseState openCollapsed=ExpandCollapseState_LeafNode;
            succeeded(galleryExpansionState(window.handle,L"Open",openCollapsed,true),"Close real Open-with item gallery");pump();
            require(openCollapsed==ExpandCollapseState_Collapsed,"Native Open-with gallery remained expanded");
            Variant initialSource;succeeded(ribbon.framework()->GetUICommandProperty(explorer::RibbonCopyMenu,UI_PKEY_ItemsSource,&initialSource.value),"Initially empty native destination source");
            Microsoft::WRL::ComPtr<IUICollection> initialCommands;
            succeeded(initialSource.value.punkVal->QueryInterface(IID_PPV_ARGS(&initialCommands)),"Initial native collection interface");
            UINT initialCount=99;succeeded(initialCommands->GetCount(&initialCount),"Initially empty native collection count");
            require(initialCount==0,"Native destination source was not initially empty");
            copySourceReady=true;callbackChangePending=true;
            succeeded(ribbon.invalidateItems(explorer::RibbonCopyMenu),"Refresh initially empty observed native collection");succeeded(ribbon.flush(),"Copy-to source-only invalidation");pump();
            ExpandCollapseState before=ExpandCollapseState_LeafNode,expanded=ExpandCollapseState_LeafNode,collapsed=ExpandCollapseState_LeafNode;
            succeeded(galleryExpansionState(window.handle,L"Copy to",before),"Native Copy-to initial expanded-state readback");
            succeeded(expandOwnedGallery(window.handle,L"Copy to"),"Expand native Copy-to gallery without input injection");pump();
            Variant source;succeeded(ribbon.framework()->GetUICommandProperty(explorer::RibbonCopyMenu,UI_PKEY_ItemsSource,&source.value),"Installed Copy-to command gallery");
            Microsoft::WRL::ComPtr<IUICollection> commands;succeeded(source.value.punkVal->QueryInterface(IID_PPV_ARGS(&commands)),"Native dynamic command collection");
            UINT count=0;succeeded(commands->GetCount(&count),"Native dynamic command count");if(count!=2)std::cerr<<"Dynamic count="<<count<<" source queries="<<copyItemsQueries<<'\n';require(count==2,"Dynamic provider rows disappeared");
            succeeded(ribbon.flush(),"Realize dynamic command properties");pump();
            require(!callbackChangePending&&callbackInvalidation==S_OK&&callbackFlush==E_PENDING,"Property callback did not defer its nested Ribbon refresh");
            Variant callbackCopy;succeeded(ribbon.framework()->GetUICommandProperty(explorer::Copy,UI_PKEY_Enabled,&callbackCopy.value),"Post-callback actual command refresh");
            BOOL callbackEnabled=FALSE;succeeded(PropVariantToBoolean(callbackCopy.value,&callbackEnabled),"Post-callback enabled value");
            require(callbackEnabled,"Queued state change was lost after the collection callback returned");
            copyEnabled=false;succeeded(ribbon.invalidateState(explorer::Copy),"Restore state after callback regression");succeeded(ribbon.flush(),"Restore callback regression state");
            for(UINT index=0;index<count;++index) {
                Microsoft::WRL::ComPtr<IUnknown> raw;Microsoft::WRL::ComPtr<IUISimplePropertySet> item;
                succeeded(commands->GetItem(index,&raw),"Dynamic provider row");succeeded(raw.As(&item),"Dynamic command identity");
                Variant id;succeeded(item->GetValue(UI_PKEY_CommandId,&id.value),"Dynamic native command ID");ULONG command=0;succeeded(PropVariantToUInt32(id.value,&command),"Dynamic command ID type");
                require(command>=0x9000&&command<=0xfffe,"Dynamic command escaped the native 16-bit command range");
                Variant enabled;succeeded(ribbon.framework()->GetUICommandProperty(command,UI_PKEY_Enabled,&enabled.value),"Dynamic native provider enabled readback");BOOL value=FALSE;succeeded(PropVariantToBoolean(enabled.value,&value),"Dynamic enabled type");require((value!=FALSE)==(index==0),"Dynamic provider disabled state was ignored");
            }
            succeeded(checkOwnedGalleryRow(window.handle,L"Owned disabled destination",false,false),"Actual native disabled destination row");
            succeeded(galleryExpansionState(window.handle,L"Copy to",expanded),"Native Copy-to state after actual visible rows");
            succeeded(galleryExpansionState(window.handle,L"Copy to",collapsed,true),"Native Copy-to collapse readback");pump();
            std::cout<<"Native Copy-to ExpandCollapse states: before="<<before<<" after-visible-rows="<<expanded<<" after-collapse="<<collapsed<<'\n';
            require(before==ExpandCollapseState_Collapsed&&collapsed==ExpandCollapseState_Collapsed,"Native Copy-to popup did not collapse");
            succeeded(expandOwnedGallery(window.handle,L"Copy to"),"Reopen native Copy-to retained destination gallery");pump();
            succeeded(checkOwnedGalleryRow(window.handle,L"Owned destination group",true,true),"Actual native destination cascade dispatch");
            require(itemExecutions==1&&lastItemCommand==explorer::RibbonCopyMenu&&lastItem==17,"Native gallery lost the retained provider path token");
        }
        Variant enabled;succeeded(ribbon.framework()->GetUICommandProperty(explorer::Copy,UI_PKEY_Enabled,&enabled.value),"Copy disabled readback");
        BOOL enabledFlag=TRUE;succeeded(PropVariantToBoolean(enabled.value,&enabledFlag),"Copy enabled type");require(!enabledFlag,"Disabled command ignored");
        copyEnabled=true;succeeded(ribbon.invalidate(explorer::Copy),"Copy state invalidation");succeeded(ribbon.flush(),"Copy flush");
        Variant changed;succeeded(ribbon.framework()->GetUICommandProperty(explorer::Copy,UI_PKEY_Enabled,&changed.value),"Copy enabled readback");succeeded(PropVariantToBoolean(changed.value,&enabledFlag),"Enabled state type");require(enabledFlag,"Changed command stayed disabled");
        Microsoft::WRL::ComPtr<IUIImage> image;succeeded(ribbon.commandImage(explorer::Copy,true,&image),"Exact native Copy resource image");require(image!=nullptr,"Native command icon missing");HBITMAP imageBitmap=nullptr;succeeded(image->GetBitmap(&imageBitmap),"Native Copy image bitmap");BITMAP imageInfo{};require(GetObjectW(imageBitmap,sizeof(imageInfo),&imageInfo)==sizeof(imageInfo)&&imageInfo.bmWidth>=32,"Native icon size differs");
        std::wstring label;succeeded(ribbon.commandLabel(explorer::Copy,label),"Native Copy localized label");require(!label.empty(),"Native label missing");
        succeeded(ribbon.selectTab(explorer::RibbonViewTab),"Select native View tab");succeeded(ribbon.flush(),"View tab flush");pump();
        Variant layout;succeeded(ribbon.framework()->GetUICommandProperty(explorer::RibbonLayoutGallery,UI_PKEY_ItemsSource,&layout.value),"Eight native layout choices");
        Microsoft::WRL::ComPtr<IUICollection> collection;succeeded(layout.value.punkVal->QueryInterface(IID_PPV_ARGS(&collection)),"Layout collection");UINT count=0;succeeded(collection->GetCount(&count),"Layout count");require(count==8,"Not all eight view modes exposed");
        if(stock) {
            const auto selectedLayout=[&] {
                Variant selected;ULONG index=UI_COLLECTION_INVALIDINDEX;
                succeeded(ribbon.nativeFramework()->GetUICommandProperty(ribbon.nativeCommandId(explorer::RibbonLayoutGallery),UI_PKEY_SelectedItem,&selected.value),"Native selected layout readback");
                succeeded(PropVariantToUInt32(selected.value,&index),"Native selected layout type");
                std::cout<<"Native selected layout: command="<<ribbon.nativeCommandId(explorer::RibbonLayoutGallery)<<" index="<<index<<'\n';
                return index;
            };
            require(selectedLayout()==5,"Native gallery lost the actual Details selection");
            succeeded(ribbon.invalidateItems(explorer::RibbonLayoutGallery),"Refresh native layout choices");
            succeeded(ribbon.flush(),"Refreshed layout choices flush");pump();
            require(selectedLayout()==5,"Replacing native layout choices cleared Details selection");
        }
        for(UINT index=0;index<count;++index) {
            Microsoft::WRL::ComPtr<IUnknown> raw;Microsoft::WRL::ComPtr<IUISimplePropertySet> item;
            succeeded(collection->GetItem(index,&raw),"Native layout item");succeeded(raw.As(&item),"Layout item properties");
            Variant picture;succeeded(item->GetValue(UI_PKEY_ItemImage,&picture.value),"Native layout image");
            require(picture.value.vt==VT_UNKNOWN&&picture.value.punkVal,"Layout image disappeared");
            Variant id;succeeded(item->GetValue(UI_PKEY_CommandId,&id.value),"Native view item command");ULONG command=0;
            succeeded(PropVariantToUInt32(id.value,&command),"Native view command type");require(command==explorer::ViewFirst+index,"Native view order changed");
        }
        const auto sceneCapture=[&](const wchar_t* name,int width,int height,explorer::RibbonContext context,UINT tab,bool below){
            succeeded(ribbon.setContexts(context,true),"Scene contextual availability");
            succeeded(ribbon.setQuickAccessBelow(below),"Scene QAT placement");
            if(context==explorer::RibbonContext::None)succeeded(ribbon.selectTab(tab),"Scene core tab");
            MoveWindow(window.handle,0,0,width+14,height+7,TRUE);pump();
            const auto scenePath=std::filesystem::current_path()/L"artifacts"/(std::wstring(L"ribbon-native-")+name+L"-"+std::to_wstring(GetCurrentProcessId())+L".png");
            explorer::VisualCaptureReport report;explorer::VisualCaptureOptions options;options.trimInvisibleFrame=true;
            succeeded(explorer::captureWindowPng(desktop,window.handle,scenePath,options,report),"Source-sized native Ribbon scene");
            auto reportPath=scenePath;reportPath.replace_extension(L".json");succeeded(explorer::writeVisualCaptureReport(reportPath,report),"Source-sized native Ribbon inventory");
        };
        sceneCapture(L"view",854,649,explorer::RibbonContext::None,explorer::RibbonViewTab,false);
        sceneCapture(L"share",856,513,explorer::RibbonContext::None,explorer::RibbonShareTab,false);
        sceneCapture(L"search",710,394,explorer::RibbonContext::Search,explorer::RibbonSearchTab,false);
        sceneCapture(L"library",820,771,explorer::RibbonContext::Library,explorer::RibbonLibraryTab,true);
        sceneCapture(L"picture",829,592,explorer::RibbonContext::Picture,explorer::RibbonPictureTab,true);
        sceneCapture(L"compressed",792,503,explorer::RibbonContext::Compressed,explorer::RibbonCompressedTab,false);
        Variant destinations;succeeded(ribbon.framework()->GetUICommandProperty(explorer::RibbonExtractToGallery,UI_PKEY_ItemsSource,&destinations.value),"Retained destination gallery items");
        Microsoft::WRL::ComPtr<IUICollection> destinationItems;succeeded(destinations.value.punkVal->QueryInterface(IID_PPV_ARGS(&destinationItems)),"Destination gallery collection");
        UINT destinationCount=0;succeeded(destinationItems->GetCount(&destinationCount),"Destination overflow count");require(destinationCount==54,"Destination gallery lost overflow items");
        Microsoft::WRL::ComPtr<IUnknown> firstDestination;Microsoft::WRL::ComPtr<IUISimplePropertySet> firstDestinationProperties;
        succeeded(destinationItems->GetItem(0,&firstDestination),"Destination image item");succeeded(firstDestination.As(&firstDestinationProperties),"Destination item properties");
        Variant category;succeeded(firstDestinationProperties->GetValue(UI_PKEY_CategoryId,&category.value),"Uncategorized item contract");
        ULONG categoryIndex=0;succeeded(PropVariantToUInt32(category.value,&categoryIndex),"Category type");require(categoryIndex==UI_COLLECTION_INVALIDINDEX,"Uncategorized gallery item has a fake category");
        succeeded(ribbon.setContexts(explorer::RibbonContext::None),"Scene context cleanup");succeeded(ribbon.setQuickAccessBelow(false),"Scene QAT cleanup");MoveWindow(window.handle,0,0,1000,700,TRUE);pump();
        std::vector<UINT> qat;succeeded(ribbon.quickAccessCommands(qat),"Native QAT readback");require(qat==std::vector<UINT>{explorer::Properties,explorer::NewFolder},"Windows 10 default QAT differs");
        const std::array<UINT,3> custom{explorer::Copy,explorer::Properties,explorer::NewFolder};succeeded(ribbon.setQuickAccessCommands(custom),"Custom native QAT");
        succeeded(ribbon.quickAccessCommands(qat),"Custom QAT readback");require(qat==std::vector<UINT>(custom.begin(),custom.end()),"QAT order changed");
        const std::array<UINT,2> duplicate{explorer::Copy,explorer::Copy};require(ribbon.setQuickAccessCommands(duplicate)==E_INVALIDARG,"Duplicate QAT accepted");
        const std::array<UINT,1> group{explorer::RibbonHomeTab};require(ribbon.setQuickAccessCommands(group)==E_INVALIDARG,"Structural QAT command accepted");
        bool below=false;succeeded(ribbon.setQuickAccessBelow(true),"Native QAT below");succeeded(ribbon.quickAccessBelow(below),"QAT placement readback");require(below,"Below-ribbon QAT ignored");
        succeeded(ribbon.setContexts(explorer::RibbonContext::Search|explorer::RibbonContext::Picture),"Multiple native contextual pages");
        Variant context;succeeded(ribbon.framework()->GetUICommandProperty(explorer::RibbonSearchContext,UI_PKEY_ContextAvailable,&context.value),"Search availability");ULONG available=0;succeeded(PropVariantToUInt32(context.value,&available),"Context type");require(available==UI_CONTEXTAVAILABILITY_AVAILABLE,"Search context missing");
        succeeded(ribbon.setContexts(explorer::RibbonContext::None),"Context cleanup");
        succeeded(ribbon.setComputerMode(true),"Native Computer mode");
        succeeded(ribbon.selectTab(explorer::RibbonComputerTab),"Native Computer selection");pump();succeeded(ribbon.flush(),"Computer body realization");
        for(const auto id:{explorer::RibbonAddNetworkLocation,explorer::RibbonAccessMedia,explorer::RibbonManageComputer}) {
            Variant property;succeeded(ribbon.framework()->GetUICommandProperty(id,UI_PKEY_Enabled,&property.value),"Computer modal group command realization");
            require(property.value.vt==VT_BOOL,"Computer group has no native commands");
        }
        sceneCapture(L"computer",791,450,explorer::RibbonContext::None,explorer::RibbonComputerTab,false);
        succeeded(ribbon.setComputerMode(false),"Native Home mode");
        succeeded(ribbon.setQuickAccessBelow(true),"Restore QAT for persistence scene");
        const auto expanded=ribbon.height();succeeded(ribbon.setMinimized(true),"Native collapse");pump();bool minimized=false;succeeded(ribbon.minimized(minimized),"Collapse property readback");require(minimized&&ribbon.height()<expanded,"Collapse did not reclaim host space");
        const auto temporary=std::filesystem::temp_directory_path()/(L"WindowsExplorer-Ribbon-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        const auto settings=temporary/L"\u8a2d\u5b9a.bin";
        succeeded(ribbon.saveSettings(settings),"Atomic Unicode native settings save");
        succeeded(ribbon.setMinimized(false),"Native expand");succeeded(ribbon.setQuickAccessBelow(false),"QAT restore top");
        const std::array<UINT,1> reset{explorer::NewFolder};succeeded(ribbon.setQuickAccessCommands(reset),"QAT reset");
        succeeded(ribbon.loadSettings(settings),"Native settings restore");pump();
        succeeded(ribbon.minimized(minimized),"Restored collapse");succeeded(ribbon.quickAccessBelow(below),"Restored QAT placement");succeeded(ribbon.quickAccessCommands(qat),"Restored QAT commands");
        if(!(minimized&&below&&qat==std::vector<UINT>(custom.begin(),custom.end())))std::cerr<<"Settings readback minimized="<<minimized<<" below="<<below<<" commands="<<qat.size()<<'\n';
        require(minimized&&below&&qat==std::vector<UINT>(custom.begin(),custom.end()),"Native persisted state lost");
        const auto invalid=temporary/L"invalid.bin";{std::ofstream stream(invalid,std::ios::binary);stream<<"not Ribbon settings";}
        require(FAILED(ribbon.loadSettings(invalid)),"Malformed native settings accepted");succeeded(ribbon.quickAccessCommands(qat),"Failed settings preserved QAT");require(qat==std::vector<UINT>(custom.begin(),custom.end()),"Failed load changed QAT");
        HRESULT wrongThread=S_OK;std::thread wrong([&]{wrongThread=ribbon.invalidate();});wrong.join();require(wrongThread==RPC_E_WRONG_THREAD,"Cross-thread framework call accepted");
        require(executions==0&&lastCommand==0,"Headless initialization invoked a user operation");
        bool visibleInput=true;succeeded(desktop.visibleWindowsOnInputDesktop(visibleInput),"Input desktop window guard");require(!visibleInput,"Native Ribbon created a visible input desktop window");
        ribbon.reset();require(!ribbon.valid()&&!ribbon.height(),"Native view not released");
        std::error_code ignored;std::filesystem::remove_all(temporary,ignored);
        std::cout<<"PASS: native Windows 10 Ribbon, exact command resources, all view modes, context, QAT, persistence, isolation\n";
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';result=1;}
    OleUninitialize();return result;
}
