#include "explorer/ribbon.hpp"
#include "explorer/commands.hpp"
#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include "state_file_security_fixture.hpp"
#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <uiautomation.h>
#include <future>
#include <chrono>
#include <cstring>
#include <shlobj.h>
#include <shlwapi.h>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void succeeded(HRESULT result,const char* message){if(FAILED(result))throw std::runtime_error(std::string(message)+" HRESULT="+std::to_string(static_cast<ULONG>(result)));}
void pump(){const auto end=GetTickCount64()+150;do{MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}while(GetTickCount64()<end);}
struct Window{HWND handle=nullptr;~Window(){if(handle)DestroyWindow(handle);}};
struct Variant{PROPVARIANT value{};~Variant(){PropVariantClear(&value);}};
struct Icon {
    HICON handle=nullptr;
    ~Icon(){if(handle)DestroyIcon(handle);}
};
struct IconRaster {
    HBITMAP bitmap=nullptr;
    HDC dc=nullptr;
    HGDIOBJ previous=nullptr;
    ~IconRaster(){
        if(dc){if(previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);DeleteDC(dc);}
        if(bitmap)DeleteObject(bitmap);
    }
};
std::vector<BYTE> bitmapPixels(HBITMAP bitmap,UINT pixels) {
    require(GdiFlush()!=FALSE,"Flush native icon raster before reading pixels");
    DIBSECTION section{};
    require(bitmap&&GetObjectW(bitmap,sizeof(section),&section)==sizeof(section),"Read actual native icon DIB section");
    const auto& actual=section.dsBm;
    require(actual.bmWidth==static_cast<LONG>(pixels)&&actual.bmHeight==static_cast<LONG>(pixels)&&actual.bmBitsPixel==32&&actual.bmPlanes==1&&
        actual.bmWidthBytes==static_cast<LONG>(pixels*4)&&actual.bmBits&&section.dsBmih.biBitCount==32&&
        section.dsBmih.biCompression==BI_RGB&&section.dsBmih.biPlanes==1&&
        (section.dsBmih.biHeight==static_cast<LONG>(pixels)||section.dsBmih.biHeight==-static_cast<LONG>(pixels)),
        "Native icon DIB dimensions, stride, orientation, or format changed");
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(pixels);
    info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    IconRaster readback;readback.dc=CreateCompatibleDC(nullptr);
    require(readback.dc!=nullptr,"Create native icon readback DC");
    std::vector<BYTE> bytes(static_cast<size_t>(pixels)*pixels*4);
    require(GetDIBits(readback.dc,bitmap,0,pixels,bytes.data(),&info,DIB_RGB_COLORS)==static_cast<int>(pixels),
        "Read every native icon bitmap scanline");
    const auto* stored=static_cast<const BYTE*>(actual.bmBits);
    std::vector<BYTE> raw(bytes.size());
    // Both the production ownership-transferred bitmap and this independent
    // raster are created with negative height. GetObject reports positive
    // height on the observed native implementation, so do not infer storage
    // orientation from that returned sign. Require its actual raw RGB rows to
    // equal the independently requested top-down GetDIBits rows before using
    // all four stored bytes, including alpha, for the exact comparison.
    std::memcpy(raw.data(),stored,raw.size());
    for(size_t pixel=0;pixel<raw.size();pixel+=4)
        require(std::memcmp(bytes.data()+pixel,raw.data()+pixel,3)==0,"Native top-down GetDIBits RGB readback differs from stored pixels");
    return raw;
}
std::vector<BYTE> iconPixels(HICON icon,UINT pixels) {
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(pixels);
    info.bmiHeader.biHeight=-static_cast<LONG>(pixels);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    IconRaster raster;void* bits=nullptr;
    raster.bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    require(raster.bitmap&&bits,"Create independent native icon raster");
    raster.dc=CreateCompatibleDC(nullptr);require(raster.dc!=nullptr,"Create independent native icon drawing DC");
    raster.previous=SelectObject(raster.dc,raster.bitmap);
    require(raster.previous&&raster.previous!=HGDI_ERROR,"Select independent native icon raster");
    std::memset(bits,0,static_cast<size_t>(pixels)*pixels*4);
    require(DrawIconEx(raster.dc,0,0,icon,static_cast<int>(pixels),static_cast<int>(pixels),0,nullptr,DI_NORMAL)!=FALSE,
        "Draw independently extracted native icon");
    const auto restored=SelectObject(raster.dc,raster.previous);
    require(restored&&restored!=HGDI_ERROR,"Deselect independent native icon raster before reading pixels");
    raster.previous=nullptr;
    return bitmapPixels(raster.bitmap,pixels);
}
std::vector<BYTE> singleIconPixels(const std::wstring& path,int resource,UINT pixels) {
    Icon icon;
    succeeded(SHDefExtractIconW(path.c_str(),resource,0,&icon.handle,nullptr,MAKELONG(pixels,0)),
        "Independent original single-size native icon extraction");
    require(icon.handle!=nullptr,"Original single-size native icon is missing");
    return iconPixels(icon.handle,pixels);
}
std::vector<BYTE> ribbonImagePixels(IUIImage* image,UINT pixels) {
    require(image!=nullptr,"Actual Ribbon cached native image is missing");
    HBITMAP bitmap=nullptr;succeeded(image->GetBitmap(&bitmap),"Get actual Ribbon cached bitmap");
    return bitmapPixels(bitmap,pixels);
}
void nativeIconCacheEquivalence(explorer::NativeRibbon& ribbon,HWND window) {
    // All extraction and image/cache calls stay on the fixture's creator STA.
    const auto* desktop=explorer::PrivateDesktop::current();
    require(desktop!=nullptr,"Native icon comparison lacks its private desktop");
    succeeded(desktop->verifyIsolation(),"Native icon comparison isolation");
    require(GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId(),"Native icon comparison left its creator thread");
    std::array<wchar_t,32768> system{};
    const auto characters=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
    require(characters&&characters<system.size(),"Resolve real system icon modules");
    struct Source {const wchar_t* module;int resource;};
    // The original measured 23 startup specifications, independent of the
    // candidate's caches and extraction/conversion implementation.
    constexpr std::array<Source,23> sources{{
        {L"imageres.dll",-5315},{L"imageres.dll",-5311},{L"shell32.dll",-240},{L"imageres.dll",-5367},
        {L"shell32.dll",-319},{L"shell32.dll",-242},{L"imageres.dll",-5100},{L"shell32.dll",-243},
        {L"shell32.dll",-16763},{L"shell32.dll",-16762},{L"imageres.dll",-5302},{L"imageres.dll",-5301},
        {L"imageres.dll",-5303},{L"imageres.dll",-5304},{L"imageres.dll",-5307},{L"imageres.dll",-5340},
        {L"imageres.dll",-5306},{L"imageres.dll",-5353},{L"imageres.dll",-5308},{L"imageres.dll",-5309},
        {L"imageres.dll",-5310},{L"imageres.dll",-8208},{L"imageres.dll",-99}
    }};
    const UINT dpi=GetDpiForWindow(window);require(dpi!=0,"Read real Ribbon HWND DPI");
    const UINT largePixels=static_cast<UINT>(MulDiv(32,static_cast<int>(dpi),96));
    const UINT smallPixels=static_cast<UINT>(MulDiv(16,static_cast<int>(dpi),96));
    size_t publicBytes=0,requestedPairBytes=0;
    for(const auto& source:sources) {
        const auto path=(std::filesystem::path(system.data())/source.module).wstring();
        const auto largeReference=singleIconPixels(path,source.resource,largePixels);
        const auto smallReference=singleIconPixels(path,source.resource,smallPixels);
        for(const bool largeFirst:{true,false}) {
            // Dot-qualified absolute spellings make test keys independent of
            // startup specifications. Each order uses a distinct cold key.
            const auto qualified=(std::filesystem::path(system.data())/(largeFirst?L".":L".\\.")/source.module).wstring();
            const auto specification=L"\""+qualified+L"\","+std::to_wstring(source.resource);
            std::array<Microsoft::WRL::ComPtr<IUIImage>,2> images;
            for(const bool large:{largeFirst,!largeFirst}) {
                const size_t index=large?0:1;
                succeeded(ribbon.itemImage(specification,large,&images[index]),"Actual cold-order Ribbon item image");
                const auto bytes=ribbonImagePixels(images[index].Get(),large?largePixels:smallPixels);
                require(bytes==(large?largeReference:smallReference),"Actual combined/cache image differs from original single-size pixels");
                publicBytes+=bytes.size();
            }
            for(int repeat=0;repeat<2;++repeat)for(const bool large:{true,false}) {
                Microsoft::WRL::ComPtr<IUIImage> cached;
                succeeded(ribbon.itemImage(specification,large,&cached),"Actual Ribbon item cache hit");
                require(cached.Get()==images[large?0:1].Get(),"Actual Ribbon item cache replaced an existing native image");
                const auto bytes=ribbonImagePixels(cached.Get(),large?largePixels:smallPixels);
                require(bytes==(large?largeReference:smallReference),"Repeated Ribbon cache hit changed a native pixel");
                publicBytes+=bytes.size();
            }
        }
        // These real native comparisons cover 125%, 150%, and 200% sizes
        // independently. Public-cache coverage above reports actual HWND DPI;
        // this fixture does not alter monitor or global DPI configuration.
        for(const auto sizes:std::array{std::pair{40U,20U},std::pair{48U,24U},std::pair{64U,32U}}) {
            Icon largeIcon,smallIcon;
            succeeded(SHDefExtractIconW(path.c_str(),source.resource,0,&largeIcon.handle,&smallIcon.handle,MAKELONG(sizes.first,sizes.second)),
                "Independent requested-pair native extraction");
            require(largeIcon.handle&&smallIcon.handle,"Requested native pair is incomplete");
            const auto largeBytes=iconPixels(largeIcon.handle,sizes.first),smallBytes=iconPixels(smallIcon.handle,sizes.second);
            require(largeBytes==singleIconPixels(path,source.resource,sizes.first)&&
                smallBytes==singleIconPixels(path,source.resource,sizes.second),"Requested native size pair changed original raster bytes");
            requestedPairBytes+=largeBytes.size()+smallBytes.size();
        }
    }
    explorer::NamespaceCommandMetadata copy;
    succeeded(explorer::namespaceCommandMetadata(explorer::ribbonCommandStoreName(explorer::Copy),&copy),
        "Independent native Copy icon metadata");
    require(!copy.icon.empty(),"Native Copy metadata has no icon");
    auto copyLocation=copy.icon;const int copyResource=PathParseIconLocationW(copyLocation.data());
    copyLocation.resize(wcslen(copyLocation.c_str()));
    std::array<wchar_t,32768> expandedCopy{};
    const auto copyCharacters=ExpandEnvironmentStringsW(copyLocation.c_str(),expandedCopy.data(),static_cast<DWORD>(expandedCopy.size()));
    require(copyCharacters&&copyCharacters<=expandedCopy.size(),"Expand independent native Copy icon location");
    for(const bool large:{true,false}) {
        Microsoft::WRL::ComPtr<IUIImage> command,item,repeated;
        succeeded(ribbon.commandImage(explorer::Copy,large,&command),"Actual public Copy command image");
        succeeded(ribbon.itemImage(copy.icon,large,&item),"Actual Copy metadata item image");
        succeeded(ribbon.commandImage(explorer::Copy,large,&repeated),"Actual public Copy command cache hit");
        require(command.Get()==item.Get()&&command.Get()==repeated.Get(),"Native command and item image paths use different caches");
        require(ribbonImagePixels(command.Get(),large?largePixels:smallPixels)==
            singleIconPixels(expandedCopy.data(),copyResource,large?largePixels:smallPixels),"Native Copy cache differs from original single-size raster");
    }
    const auto missing=(std::filesystem::temp_directory_path()/
        (L"WindowsExplorer-MissingIcon-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()))/L"absent.dll").wstring();
    require(GetFileAttributesW(missing.c_str())==INVALID_FILE_ATTRIBUTES,"Missing native icon fixture already exists");
    const auto missingError=GetLastError();
    require(missingError==ERROR_FILE_NOT_FOUND||missingError==ERROR_PATH_NOT_FOUND,"Missing native icon fixture failed for a different reason");
    const std::array failures{std::pair{(std::filesystem::path(system.data())/L"shell32.dll").wstring(),-65535},std::pair{missing,-1}};
    for(const auto& [path,resource]:failures)for(const bool large:{true,false}) {
        Icon reference;const auto status=SHDefExtractIconW(path.c_str(),resource,0,&reference.handle,nullptr,MAKELONG(large?largePixels:smallPixels,0));
        require(FAILED(status)||!reference.handle,"Missing native resource/file unexpectedly yielded an icon");
        const auto expected=FAILED(status)?status:E_FAIL;
        const auto specification=L"\""+path+L"\","+std::to_wstring(resource);
        for(int repeat=0;repeat<2;++repeat) {
            Microsoft::WRL::ComPtr<IUIImage> image;
            require(ribbon.itemImage(specification,large,&image)==expected&&!image,
                "Actual missing icon/file changed original single-size HRESULT or output");
        }
    }
    std::cout<<"PASS: 23 native icon public caches at HWND DPI="<<dpi<<" ("<<largePixels<<'/'<<smallPixels
        <<"), both cold request orders and repeated hits; "<<publicBytes<<" exact public bytes; independent 40/20, 48/24, 64/32 pairs "
        <<requestedPairBytes<<" exact bytes; missing resource/file HRESULTs\n";
}
class LabelReferenceHandler final : public IUIApplication, public IUICommandHandler {
public:
    std::map<UINT,std::wstring> labels,tooltips;
    std::map<UINT,UINT> labelRequests;
    std::map<UINT,UI_COMMANDTYPE> commandTypes;
    std::map<UINT,VARTYPE> firstLabelTypes;
    std::map<UINT,std::wstring> firstLabels;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(id==IID_IUnknown||id==__uuidof(IUIApplication))*output=static_cast<IUIApplication*>(this);
        else if(id==__uuidof(IUICommandHandler))*output=static_cast<IUICommandHandler*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE OnViewChanged(UINT32,UI_VIEWTYPE,IUnknown*,UI_VIEWVERB verb,INT32 reason) override {
        return verb==UI_VIEWVERB_ERROR?reason:S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCreateUICommand(UINT32 command,UI_COMMANDTYPE type,IUICommandHandler** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        try {commandTypes.insert_or_assign(command,type);}catch(...){return E_OUTOFMEMORY;}
        *output=static_cast<IUICommandHandler*>(this);AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDestroyUICommand(UINT32,UI_COMMANDTYPE,IUICommandHandler*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE Execute(UINT32,UI_EXECUTIONVERB,const PROPERTYKEY*,const PROPVARIANT*,IUISimplePropertySet*) override {
        return E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE UpdateProperty(UINT32 command,REFPROPERTYKEY key,const PROPVARIANT* current,PROPVARIANT* output) override {
        if(!output)return E_POINTER;PropVariantInit(output);
        try {
            if(IsEqualPropertyKey(key,UI_PKEY_Label)||IsEqualPropertyKey(key,UI_PKEY_TooltipTitle)) {
                const bool label=IsEqualPropertyKey(key,UI_PKEY_Label);
                if(label) {
                    ++labelRequests[command];
                    const auto first=firstLabelTypes.try_emplace(command,current?current->vt:static_cast<VARTYPE>(VT_EMPTY)).second;
                    if(first&&current&&current->vt==VT_LPWSTR&&current->pwszVal)
                        firstLabels.emplace(command,current->pwszVal);
                }
                if(current&&current->vt==VT_LPWSTR&&current->pwszVal&&*current->pwszVal) {
                    (label?labels:tooltips).try_emplace(command,current->pwszVal);
                    return PropVariantCopy(output,current);
                }
            }
        }catch(...){return E_OUTOFMEMORY;}
        return E_NOTIMPL;
    }
private:
    std::atomic<ULONG> references_{1};
};
struct InstalledLabelReference {
    Window window;
    HMODULE module=nullptr;
    Microsoft::WRL::ComPtr<LabelReferenceHandler> handler;
    Microsoft::WRL::ComPtr<IUIFramework> framework;
    ~InstalledLabelReference() {
        if(framework)framework->Destroy();framework.Reset();handler.Reset();
        if(module)FreeLibrary(module);
    }
    void initialize(const wchar_t* windowClass,const explorer::RibbonFeatures& features) {
        window.handle=CreateWindowExW(0,windowClass,L"Owned native Ribbon label reference",WS_OVERLAPPEDWINDOW,
            0,0,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window.handle!=nullptr,"Create private native label-reference host");
        module=LoadLibraryExW(L"ExplorerFrame.dll",nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        require(module!=nullptr,"Load actual installed Ribbon resource module");
        handler.Attach(new LabelReferenceHandler());
        succeeded(CoCreateInstance(CLSID_UIRibbonFramework,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&framework)),
            "Create independent native label-reference framework");
        succeeded(framework->Initialize(window.handle,handler.Get()),"Initialize independent native label reference");
        succeeded(framework->LoadUI(module,L"EXPLORER_RIBBON"),"Load unmodified installed BML label resources");
        succeeded(framework->SetModes(0xa1|(features.discBurning?0x20000:0x40000)),"Select actual native Home reference mode");
        require(!IsWindowVisible(window.handle),"Native label-reference host became visible");
    }
};
struct OwnedRefineControl {
    std::wstring name;
    CONTROLTYPEID type=0;
    RECT bounds{};
};
struct OwnedRefineLabels {
    std::wstring selectedTab,toolbar;
    CONTROLTYPEID parentType=0;
    std::vector<OwnedRefineControl> controls;
    std::vector<std::wstring> groupLabels;
};
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
std::string diagnosticCaption(std::wstring_view caption) {
    if(caption.empty())return {};
    const auto characters=static_cast<int>(caption.size());
    const auto bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,caption.data(),characters,nullptr,0,nullptr,nullptr);
    if(!bytes)return "<invalid UTF-16>";
    std::string result(static_cast<size_t>(bytes),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,caption.data(),characters,result.data(),bytes,nullptr,nullptr))
        return "<invalid UTF-16>";
    return result;
}
void diagnoseOwnedLabel(IUIAutomation* automation,HWND window,const std::wstring& expected,HRESULT lookup) {
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> matches;
    auto hr=automation->ElementFromHandle(window,&root);
    VARIANT caption{};caption.vt=VT_BSTR;caption.bstrVal=SysAllocString(expected.c_str());
    if(SUCCEEDED(hr))hr=automation->CreatePropertyCondition(UIA_NamePropertyId,caption,&condition);
    VariantClear(&caption);
    if(SUCCEEDED(hr))hr=root->FindAll(TreeScope_Descendants,condition.Get(),&matches);
    int count=-1;if(SUCCEEDED(hr))hr=matches->get_Length(&count);
    std::cout<<"Owned UIA label: expected=\""<<diagnosticCaption(expected)<<"\" count="<<count
        <<" lookup="<<static_cast<ULONG>(lookup)<<" enumeration="<<static_cast<ULONG>(hr)<<'\n';
    for(int index=0;SUCCEEDED(hr)&&index<count;++index) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> match;
        const auto item=matches->GetElement(index,&match);
        if(FAILED(item)){std::cout<<"  match="<<index<<" HRESULT="<<static_cast<ULONG>(item)<<'\n';continue;}
        CONTROLTYPEID type=0;BOOL offscreen=TRUE,enabled=FALSE;RECT bounds{};
        const auto typeRead=match->get_CurrentControlType(&type);
        const auto offscreenRead=match->get_CurrentIsOffscreen(&offscreen);
        const auto enabledRead=match->get_CurrentIsEnabled(&enabled);
        const auto boundsRead=match->get_CurrentBoundingRectangle(&bounds);
        std::cout<<"  match="<<index<<" type="<<type<<" offscreen="<<offscreen<<" enabled="<<enabled
            <<" bounds="<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom
            <<" propertyStatus="<<static_cast<ULONG>(typeRead)<<','<<static_cast<ULONG>(offscreenRead)<<','
            <<static_cast<ULONG>(enabledRead)<<','<<static_cast<ULONG>(boundsRead)<<'\n';
    }
}
void diagnoseOwnedRibbonTabs(IUIAutomation* automation,HWND window) {
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> descendants;
    auto hr=automation->ElementFromHandle(window,&root);
    if(SUCCEEDED(hr))hr=automation->CreateTrueCondition(&condition);
    if(SUCCEEDED(hr))hr=root->FindAll(TreeScope_Descendants,condition.Get(),&descendants);
    int count=-1;if(SUCCEEDED(hr))hr=descendants->get_Length(&count);
    std::cout<<"Owned UIA Ribbon tree: descendants="<<count<<" HRESULT="<<static_cast<ULONG>(hr)<<'\n';
    int printed=0;
    for(int index=0;SUCCEEDED(hr)&&index<count&&printed<80;++index) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> item;
        CONTROLTYPEID type=0;
        if(FAILED(descendants->GetElement(index,&item))||FAILED(item->get_CurrentControlType(&type)))continue;
        if(type!=UIA_TabControlTypeId&&type!=UIA_TabItemControlTypeId&&type!=UIA_GroupControlTypeId&&
           type!=UIA_ToolBarControlTypeId)continue;
        ++printed;BSTR name=nullptr;BOOL offscreen=TRUE;RECT bounds{};
        const auto nameRead=item->get_CurrentName(&name);
        const auto offscreenRead=item->get_CurrentIsOffscreen(&offscreen);
        const auto boundsRead=item->get_CurrentBoundingRectangle(&bounds);
        Microsoft::WRL::ComPtr<IUIAutomationSelectionItemPattern> selection;
        auto selectedRead=item->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&selection));
        BOOL selected=FALSE;if(SUCCEEDED(selectedRead)&&selection)selectedRead=selection->get_CurrentIsSelected(&selected);
        std::cout<<"  caption=\""<<diagnosticCaption(name?std::wstring_view(name):std::wstring_view{})
            <<"\" type="<<type<<" offscreen="<<offscreen<<" selected="<<selected
            <<" bounds="<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom
            <<" propertyStatus="<<static_cast<ULONG>(nameRead)<<','<<static_cast<ULONG>(offscreenRead)<<','
            <<static_cast<ULONG>(boundsRead)<<','<<static_cast<ULONG>(selectedRead)<<'\n';
        SysFreeString(name);
    }
}
void diagnoseOwnedSearchToolbars(IUIAutomation* automation,HWND window) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> toolbars;
    ComPtr<IUIAutomationTreeWalker> walker;
    auto status=automation->ElementFromHandle(window,&root);
    VARIANT type{};type.vt=VT_I4;type.lVal=UIA_ToolBarControlTypeId;
    if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&condition);
    if(SUCCEEDED(status))status=root->FindAll(TreeScope_Descendants,condition.Get(),&toolbars);
    if(SUCCEEDED(status))status=automation->get_ControlViewWalker(&walker);
    int count=-1;if(SUCCEEDED(status))status=toolbars->get_Length(&count);
    std::cout<<"Owned raw Search toolbars: count="<<count<<" HRESULT="<<static_cast<ULONG>(status)<<'\n';
    for(int index=0;SUCCEEDED(status)&&index<count&&index<24;++index) {
        ComPtr<IUIAutomationElement> toolbar,parent;
        auto item=toolbars->GetElement(index,&toolbar);
        if(SUCCEEDED(item))item=walker->GetParentElement(toolbar.Get(),&parent);
        BSTR name=nullptr,parentName=nullptr;RECT bounds{},parentBounds{};BOOL offscreen=TRUE;CONTROLTYPEID parentType=0;
        const auto nameRead=toolbar?toolbar->get_CurrentName(&name):E_POINTER;
        const auto offscreenRead=toolbar?toolbar->get_CurrentIsOffscreen(&offscreen):E_POINTER;
        const auto boundsRead=toolbar?toolbar->get_CurrentBoundingRectangle(&bounds):E_POINTER;
        const auto parentNameRead=parent?parent->get_CurrentName(&parentName):E_POINTER;
        const auto parentTypeRead=parent?parent->get_CurrentControlType(&parentType):E_POINTER;
        const auto parentBoundsRead=parent?parent->get_CurrentBoundingRectangle(&parentBounds):E_POINTER;
        std::cout<<"  toolbar="<<index<<" caption=\""<<diagnosticCaption(name?std::wstring_view(name):std::wstring_view{})
            <<"\" type="<<UIA_ToolBarControlTypeId<<" offscreen="<<offscreen<<" bounds="<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom
            <<" parent=\""<<diagnosticCaption(parentName?std::wstring_view(parentName):std::wstring_view{})<<"\" parentType="<<parentType
            <<" parentBounds="<<parentBounds.left<<','<<parentBounds.top<<','<<parentBounds.right<<','<<parentBounds.bottom
            <<" propertyStatus="<<static_cast<ULONG>(item)<<','<<static_cast<ULONG>(nameRead)<<','<<static_cast<ULONG>(offscreenRead)<<','
            <<static_cast<ULONG>(boundsRead)<<','<<static_cast<ULONG>(parentNameRead)<<','<<static_cast<ULONG>(parentTypeRead)<<','
            <<static_cast<ULONG>(parentBoundsRead)<<'\n';
        SysFreeString(name);SysFreeString(parentName);
        if(FAILED(item)||!toolbar)continue;
        ComPtr<IUIAutomationElement> child;item=walker->GetFirstChildElement(toolbar.Get(),&child);
        for(int childIndex=0;SUCCEEDED(item)&&child&&childIndex<24;++childIndex) {
            BSTR childName=nullptr;RECT childBounds{};BOOL childOffscreen=TRUE;CONTROLTYPEID childType=0;
            const auto childNameRead=child->get_CurrentName(&childName);
            const auto childTypeRead=child->get_CurrentControlType(&childType);
            const auto childOffscreenRead=child->get_CurrentIsOffscreen(&childOffscreen);
            const auto childBoundsRead=child->get_CurrentBoundingRectangle(&childBounds);
            std::cout<<"    child="<<childIndex<<" caption=\""<<diagnosticCaption(childName?std::wstring_view(childName):std::wstring_view{})
                <<"\" type="<<childType<<" offscreen="<<childOffscreen<<" bounds="<<childBounds.left<<','<<childBounds.top<<','
                <<childBounds.right<<','<<childBounds.bottom<<" propertyStatus="<<static_cast<ULONG>(childNameRead)<<','
                <<static_cast<ULONG>(childTypeRead)<<','<<static_cast<ULONG>(childOffscreenRead)<<','<<static_cast<ULONG>(childBoundsRead)<<'\n';
            SysFreeString(childName);ComPtr<IUIAutomationElement> next;
            item=walker->GetNextSiblingElement(child.Get(),&next);child=std::move(next);
        }
    }
}
HRESULT ownedHomeGroupLabels(HWND window,std::vector<std::wstring>& output) {
    auto result=std::make_shared<std::vector<std::wstring>>();
    const auto hr=ownedAutomation(window,[window,result](IUIAutomation* automation) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> root;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition,tabCondition;
        Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker;
        auto status=automation->ElementFromHandle(window,&root);
        VARIANT type{};type.vt=VT_I4;type.lVal=UIA_ToolBarControlTypeId;
        if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&condition);
        type.lVal=UIA_TabItemControlTypeId;
        if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&tabCondition);
        if(SUCCEEDED(status))status=automation->get_ControlViewWalker(&walker);
        if(FAILED(status))return status;
        const auto deadline=GetTickCount64()+2000;
        int lastToolbarCount=0;size_t lastGroupCount=0;bool lastReady=false;
        do {
            Microsoft::WRL::ComPtr<IUIAutomationElementArray> matches;
            status=root->FindAll(TreeScope_Descendants,condition.Get(),&matches);
            int count=0;if(SUCCEEDED(status))status=matches->get_Length(&count);
            Microsoft::WRL::ComPtr<IUIAutomationElementArray> tabs;
            if(SUCCEEDED(status))status=root->FindAll(TreeScope_Descendants,tabCondition.Get(),&tabs);
            int tabCount=0;if(SUCCEEDED(status))status=tabs->get_Length(&tabCount);
            std::wstring selectedName;RECT selectedBounds{};int selectedCount=0;
            for(int index=0;SUCCEEDED(status)&&index<tabCount;++index) {
                Microsoft::WRL::ComPtr<IUIAutomationElement> tab;
                Microsoft::WRL::ComPtr<IUIAutomationSelectionItemPattern> selection;
                status=tabs->GetElement(index,&tab);
                if(SUCCEEDED(status))status=tab->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&selection));
                if(SUCCEEDED(status)&&!selection)status=E_NOINTERFACE;
                BOOL selected=FALSE;if(SUCCEEDED(status))status=selection->get_CurrentIsSelected(&selected);
                if(FAILED(status)||!selected)continue;
                ++selectedCount;BSTR name=nullptr;
                status=tab->get_CurrentName(&name);
                if(SUCCEEDED(status))status=tab->get_CurrentBoundingRectangle(&selectedBounds);
                if(SUCCEEDED(status))selectedName=name?name:L"";
                SysFreeString(name);
            }
            std::vector<std::pair<LONG,std::wstring>> groups;
            Microsoft::WRL::ComPtr<IUIAutomationElement> content;
            bool ready=selectedCount==1&&!selectedName.empty()&&selectedBounds.right>selectedBounds.left&&selectedBounds.bottom>selectedBounds.top;
            for(int index=0;SUCCEEDED(status)&&index<count;++index) {
                Microsoft::WRL::ComPtr<IUIAutomationElement> group,parent;
                status=matches->GetElement(index,&group);
                if(SUCCEEDED(status))status=walker->GetParentElement(group.Get(),&parent);
                CONTROLTYPEID parentType=0;
                if(SUCCEEDED(status)&&!parent)status=E_UNEXPECTED;
                if(SUCCEEDED(status))status=parent->get_CurrentControlType(&parentType);
                if(FAILED(status))break;
                // Native Ribbon groups are toolbars in the selected Custom
                // content panel. The Quick Access toolbar has a Pane parent and
                // must not be mistaken for a Home group.
                if(parentType!=UIA_CustomControlTypeId)continue;
                if(!content)content=parent;
                BOOL samePane=FALSE;
                status=automation->CompareElements(content.Get(),parent.Get(),&samePane);
                if(SUCCEEDED(status)&&!samePane)status=E_UNEXPECTED;
                BSTR name=nullptr,parentName=nullptr;BOOL offscreen=TRUE;RECT bounds{},parentBounds{};
                if(SUCCEEDED(status))status=group->get_CurrentName(&name);
                if(SUCCEEDED(status))status=group->get_CurrentIsOffscreen(&offscreen);
                if(SUCCEEDED(status))status=group->get_CurrentBoundingRectangle(&bounds);
                if(SUCCEEDED(status))status=parent->get_CurrentName(&parentName);
                if(SUCCEEDED(status))status=parent->get_CurrentBoundingRectangle(&parentBounds);
                if(SUCCEEDED(status)) {
                    ready=ready&&name&&*name&&!offscreen&&bounds.right>bounds.left&&bounds.bottom>bounds.top&&
                        parentName&&std::wstring_view(parentName)==selectedName&&parentBounds.top>=selectedBounds.bottom&&
                        bounds.left>=parentBounds.left&&bounds.right<=parentBounds.right&&
                        bounds.top>=parentBounds.top&&bounds.bottom<=parentBounds.bottom;
                    groups.emplace_back(bounds.left,name?name:L"");
                }
                SysFreeString(name);SysFreeString(parentName);
            }
            if(FAILED(status))return status;
            lastToolbarCount=count;lastGroupCount=groups.size();lastReady=ready;
            if(groups.size()>5)return E_UNEXPECTED;
            if(groups.size()==5&&ready) {
                std::sort(groups.begin(),groups.end(),[](const auto& left,const auto& right){return left.first<right.first;});
                result->clear();for(auto& group:groups)result->push_back(std::move(group.second));
                return S_OK;
            }
            // Realization can follow ShowWindow asynchronously. Observe the
            // same owned native pane while its owning STA pumps, until all
            // five Home groups have actual visible native bounds and names.
            Sleep(10);
        }while(GetTickCount64()<deadline);
        std::cout<<"Owned native Home realization timeout: toolbars="<<lastToolbarCount
            <<" paneGroups="<<lastGroupCount<<" ready="<<lastReady<<'\n';
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> remaining;
        const auto enumeration=root->FindAll(TreeScope_Descendants,condition.Get(),&remaining);
        int count=0;
        if(SUCCEEDED(enumeration)&&SUCCEEDED(remaining->get_Length(&count)))for(int index=0;index<count&&index<20;++index) {
            Microsoft::WRL::ComPtr<IUIAutomationElement> toolbar,parent;
            if(FAILED(remaining->GetElement(index,&toolbar)))continue;
            BSTR name=nullptr,parentName=nullptr;BOOL offscreen=TRUE;RECT bounds{},parentBounds{};
            CONTROLTYPEID parentType=0;
            const auto nameRead=toolbar->get_CurrentName(&name);
            const auto offscreenRead=toolbar->get_CurrentIsOffscreen(&offscreen);
            const auto boundsRead=toolbar->get_CurrentBoundingRectangle(&bounds);
            auto parentRead=walker->GetParentElement(toolbar.Get(),&parent);
            if(SUCCEEDED(parentRead)&&parent) {
                parentRead=parent->get_CurrentControlType(&parentType);
                if(SUCCEEDED(parentRead))parentRead=parent->get_CurrentName(&parentName);
                if(SUCCEEDED(parentRead))parentRead=parent->get_CurrentBoundingRectangle(&parentBounds);
            }
            std::cout<<"  toolbar=\""<<diagnosticCaption(name?std::wstring_view(name):std::wstring_view{})
                <<"\" offscreen="<<offscreen<<" bounds="<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom
                <<" parentType="<<parentType<<" parent=\""<<diagnosticCaption(parentName?std::wstring_view(parentName):std::wstring_view{})
                <<"\" parentBounds="<<parentBounds.left<<','<<parentBounds.top<<','<<parentBounds.right<<','<<parentBounds.bottom
                <<" propertyStatus="<<static_cast<ULONG>(nameRead)<<','<<static_cast<ULONG>(offscreenRead)<<','
                <<static_cast<ULONG>(boundsRead)<<','<<static_cast<ULONG>(parentRead)<<'\n';
            SysFreeString(name);SysFreeString(parentName);
        }
        diagnoseOwnedRibbonTabs(automation,window);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    });
    if(SUCCEEDED(hr))output=std::move(*result);
    return hr;
}
HRESULT ownedSearchRefineLabels(HWND window,OwnedRefineLabels& output) {
    struct RibbonWindow {HWND handle=nullptr;bool duplicate=false;} ribbon;
    EnumChildWindows(window,[](HWND child,LPARAM parameter)->BOOL {
        wchar_t name[64]{};
        if(GetClassNameW(child,name,static_cast<int>(std::size(name)))&&lstrcmpW(name,L"UIRibbonCommandBar")==0) {
            auto& found=*reinterpret_cast<RibbonWindow*>(parameter);
            if(found.handle)found.duplicate=true;else found.handle=child;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&ribbon));
    DWORD process=0;
    if(!ribbon.handle||ribbon.duplicate||GetWindowThreadProcessId(ribbon.handle,&process)!=GetCurrentThreadId()||
       process!=GetCurrentProcessId())return E_UNEXPECTED;
    auto result=std::make_shared<OwnedRefineLabels>();
    const auto hr=ownedAutomation(window,[handle=ribbon.handle,result](IUIAutomation* automation) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IUIAutomationElement> root;
        ComPtr<IUIAutomationCondition> toolbarCondition,tabCondition;
        ComPtr<IUIAutomationTreeWalker> walker;
        auto status=automation->ElementFromHandle(handle,&root);
        VARIANT type{};type.vt=VT_I4;type.lVal=UIA_ToolBarControlTypeId;
        if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&toolbarCondition);
        type.lVal=UIA_TabItemControlTypeId;
        if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&tabCondition);
        if(SUCCEEDED(status))status=automation->get_ControlViewWalker(&walker);
        if(FAILED(status))return status;
        const auto deadline=GetTickCount64()+2000;
        int lastGroups=0,lastSelectedCount=0;size_t lastControls=0;
        std::wstring lastSelectedName;RECT lastSelectedBounds{};
        do {
            ComPtr<IUIAutomationElementArray> tabs,toolbars;
            status=root->FindAll(TreeScope_Descendants,tabCondition.Get(),&tabs);
            int tabCount=0;if(SUCCEEDED(status))status=tabs->get_Length(&tabCount);
            std::wstring selectedName;RECT selectedBounds{};int selectedCount=0;
            for(int index=0;SUCCEEDED(status)&&index<tabCount;++index) {
                ComPtr<IUIAutomationElement> tab;ComPtr<IUIAutomationSelectionItemPattern> selection;
                status=tabs->GetElement(index,&tab);
                if(SUCCEEDED(status))status=tab->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&selection));
                if(SUCCEEDED(status)&&!selection)status=E_NOINTERFACE;
                BOOL selected=FALSE;if(SUCCEEDED(status))status=selection->get_CurrentIsSelected(&selected);
                if(FAILED(status)||!selected)continue;
                ++selectedCount;BSTR name=nullptr;status=tab->get_CurrentName(&name);
                if(SUCCEEDED(status))status=tab->get_CurrentBoundingRectangle(&selectedBounds);
                if(SUCCEEDED(status))selectedName=name?name:L"";SysFreeString(name);
            }
            if(FAILED(status))return status;
            lastSelectedCount=selectedCount;lastSelectedName=selectedName;lastSelectedBounds=selectedBounds;
            status=root->FindAll(TreeScope_Descendants,toolbarCondition.Get(),&toolbars);
            int count=0;if(SUCCEEDED(status))status=toolbars->get_Length(&count);
            struct Group {ComPtr<IUIAutomationElement> element;std::wstring name;RECT bounds{};CONTROLTYPEID parentType=0;};
            std::vector<Group> groups;ComPtr<IUIAutomationElement> content;
            for(int index=0;SUCCEEDED(status)&&index<count;++index) {
                ComPtr<IUIAutomationElement> toolbar,parent;
                status=toolbars->GetElement(index,&toolbar);
                BOOL offscreen=TRUE;if(SUCCEEDED(status))status=toolbar->get_CurrentIsOffscreen(&offscreen);
                if(FAILED(status)||offscreen)continue;
                status=walker->GetParentElement(toolbar.Get(),&parent);
                if(SUCCEEDED(status)&&!parent)status=E_UNEXPECTED;
                if(FAILED(status))break;
                BSTR name=nullptr,parentName=nullptr;RECT bounds{},parentBounds{};CONTROLTYPEID parentType=0;
                status=toolbar->get_CurrentName(&name);
                if(SUCCEEDED(status))status=toolbar->get_CurrentBoundingRectangle(&bounds);
                if(SUCCEEDED(status))status=parent->get_CurrentName(&parentName);
                if(SUCCEEDED(status))status=parent->get_CurrentBoundingRectangle(&parentBounds);
                if(SUCCEEDED(status))status=parent->get_CurrentControlType(&parentType);
                // Discover the selected Search pane's actual parent identity
                // and type. Do not assume Home's Custom parent type or mistake
                // the caption QAT for a content group.
                const bool current=SUCCEEDED(status)&&selectedCount==1&&!selectedName.empty()&&parentName&&
                    std::wstring_view(parentName)==selectedName&&
                    bounds.right>bounds.left&&bounds.bottom>bounds.top&&parentBounds.top>=selectedBounds.bottom&&
                    bounds.left>=parentBounds.left&&bounds.top>=parentBounds.top&&
                    bounds.right<=parentBounds.right&&bounds.bottom<=parentBounds.bottom;
                if(current) {
                    if(!content)content=parent;
                    BOOL same=FALSE;status=automation->CompareElements(content.Get(),parent.Get(),&same);
                    if(SUCCEEDED(status)&&!same)status=E_UNEXPECTED;
                    if(SUCCEEDED(status))groups.push_back({toolbar,name?name:L"",bounds,parentType});
                }
                SysFreeString(name);SysFreeString(parentName);
            }
            if(FAILED(status))return status;
            lastGroups=static_cast<int>(groups.size());
            // The actual native template has four groups, including the empty
            // Close caption. Native raw readback proved its second group is
            // Refine and that all four whole galleries can have empty captions.
            if(groups.size()==4) {
                std::sort(groups.begin(),groups.end(),[](const auto& left,const auto& right){return left.bounds.left<right.bounds.left;});
                auto& refine=groups[1];std::vector<OwnedRefineControl> controls;
                ComPtr<IUIAutomationElement> child;status=walker->GetFirstChildElement(refine.element.Get(),&child);
                while(SUCCEEDED(status)&&child) {
                    BSTR name=nullptr;BOOL offscreen=TRUE;RECT bounds{};CONTROLTYPEID controlType=0;
                    status=child->get_CurrentName(&name);
                    if(SUCCEEDED(status))status=child->get_CurrentIsOffscreen(&offscreen);
                    if(SUCCEEDED(status))status=child->get_CurrentBoundingRectangle(&bounds);
                    if(SUCCEEDED(status))status=child->get_CurrentControlType(&controlType);
                    if(SUCCEEDED(status)&&!offscreen&&bounds.right>bounds.left&&bounds.bottom>bounds.top&&
                       bounds.left>=refine.bounds.left&&bounds.top>=refine.bounds.top&&
                       bounds.right<=refine.bounds.right&&bounds.bottom<=refine.bounds.bottom)
                        controls.push_back({name?name:L"",controlType,bounds});
                    SysFreeString(name);
                    ComPtr<IUIAutomationElement> next;
                    if(SUCCEEDED(status))status=walker->GetNextSiblingElement(child.Get(),&next);
                    child=std::move(next);
                }
                if(FAILED(status))return status;
                lastControls=controls.size();
                if(controls.size()==4) {
                    result->selectedTab=selectedName;result->toolbar=refine.name;result->parentType=refine.parentType;
                    result->controls=std::move(controls);result->groupLabels.clear();
                    for(const auto& group:groups)result->groupLabels.push_back(group.name);
                    return S_OK;
                }
            }
            Sleep(10);
        }while(GetTickCount64()<deadline);
        std::cout<<"Owned Search Refine realization timeout: groups="<<lastGroups<<" controls="<<lastControls
            <<" selectedCount="<<lastSelectedCount<<" selectedTab=\""<<diagnosticCaption(lastSelectedName)<<"\" selectedBounds="
            <<lastSelectedBounds.left<<','<<lastSelectedBounds.top<<','<<lastSelectedBounds.right<<','<<lastSelectedBounds.bottom<<'\n';
        diagnoseOwnedRibbonTabs(automation,handle);
        diagnoseOwnedSearchToolbars(automation,handle);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    });
    if(SUCCEEDED(hr))output=std::move(*result);
    return hr;
}
void diagnoseRefineLabels(const char* source,const OwnedRefineLabels& labels) {
    std::cout<<"Owned Search Refine "<<source<<": selectedTab=\""<<diagnosticCaption(labels.selectedTab)
        <<"\" toolbar=\""<<diagnosticCaption(labels.toolbar)<<"\" parentType="<<labels.parentType
        <<" controls="<<labels.controls.size()<<'\n';
    std::cout<<"  ordered native group captions:";
    for(const auto& caption:labels.groupLabels)std::cout<<" ["<<diagnosticCaption(caption)<<']';
    std::cout<<'\n';
    for(const auto& control:labels.controls)
        std::cout<<"  caption=\""<<diagnosticCaption(control.name)<<"\" type="<<control.type
            <<" parentType="<<UIA_ToolBarControlTypeId<<" bounds="<<control.bounds.left<<','<<control.bounds.top<<','
            <<control.bounds.right<<','<<control.bounds.bottom<<'\n';
}
struct OwnedSingleGroupLabel {
    std::wstring selectedTab,caption;
    RECT bounds{};
};
HRESULT ownedSingleGroupLabel(HWND window,OwnedSingleGroupLabel& output) {
    auto result=std::make_shared<OwnedSingleGroupLabel>();
    const auto hr=ownedAutomation(window,[window,result](IUIAutomation* automation) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IUIAutomationElement> root;
        ComPtr<IUIAutomationCondition> toolbarCondition,tabCondition;
        ComPtr<IUIAutomationTreeWalker> walker;
        auto status=automation->ElementFromHandle(window,&root);
        VARIANT type{};type.vt=VT_I4;type.lVal=UIA_ToolBarControlTypeId;
        if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&toolbarCondition);
        type.lVal=UIA_TabItemControlTypeId;
        if(SUCCEEDED(status))status=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&tabCondition);
        if(SUCCEEDED(status))status=automation->get_ControlViewWalker(&walker);
        if(FAILED(status))return status;
        const auto deadline=GetTickCount64()+2000;
        do {
            ComPtr<IUIAutomationElementArray> tabs,toolbars;
            status=root->FindAll(TreeScope_Descendants,tabCondition.Get(),&tabs);
            int tabCount=0;if(SUCCEEDED(status))status=tabs->get_Length(&tabCount);
            std::wstring selectedName;RECT selectedBounds{};int selectedCount=0;
            for(int index=0;SUCCEEDED(status)&&index<tabCount;++index) {
                ComPtr<IUIAutomationElement> tab;ComPtr<IUIAutomationSelectionItemPattern> selection;
                status=tabs->GetElement(index,&tab);
                if(SUCCEEDED(status))status=tab->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(&selection));
                if(SUCCEEDED(status)&&!selection)status=E_NOINTERFACE;
                BOOL selected=FALSE;if(SUCCEEDED(status))status=selection->get_CurrentIsSelected(&selected);
                if(FAILED(status)||!selected)continue;
                ++selectedCount;BSTR rawName=nullptr;status=tab->get_CurrentName(&rawName);
                const std::unique_ptr<OLECHAR,decltype(&SysFreeString)> name(rawName,SysFreeString);
                if(SUCCEEDED(status))status=tab->get_CurrentBoundingRectangle(&selectedBounds);
                if(SUCCEEDED(status))selectedName=name?name.get():L"";
            }
            if(SUCCEEDED(status))status=root->FindAll(TreeScope_Descendants,toolbarCondition.Get(),&toolbars);
            int toolbarCount=0;if(SUCCEEDED(status))status=toolbars->get_Length(&toolbarCount);
            UINT groupCount=0;OwnedSingleGroupLabel observed;
            for(int index=0;SUCCEEDED(status)&&index<toolbarCount;++index) {
                ComPtr<IUIAutomationElement> toolbar,parent;
                status=toolbars->GetElement(index,&toolbar);
                BOOL offscreen=TRUE;if(SUCCEEDED(status))status=toolbar->get_CurrentIsOffscreen(&offscreen);
                if(FAILED(status)||offscreen)continue;
                status=walker->GetParentElement(toolbar.Get(),&parent);
                if(SUCCEEDED(status)&&!parent)status=E_UNEXPECTED;
                BSTR rawName=nullptr,rawParentName=nullptr;RECT bounds{},parentBounds{};
                if(SUCCEEDED(status))status=toolbar->get_CurrentName(&rawName);
                const std::unique_ptr<OLECHAR,decltype(&SysFreeString)> name(rawName,SysFreeString);
                if(SUCCEEDED(status))status=toolbar->get_CurrentBoundingRectangle(&bounds);
                if(SUCCEEDED(status))status=parent->get_CurrentName(&rawParentName);
                const std::unique_ptr<OLECHAR,decltype(&SysFreeString)> parentName(rawParentName,SysFreeString);
                if(SUCCEEDED(status))status=parent->get_CurrentBoundingRectangle(&parentBounds);
                // Actual selected content-pane identity and visible geometry
                // distinguish its group from the caption's Quick Access bar.
                // Empty is a valid native group caption, not missing evidence.
                const bool current=SUCCEEDED(status)&&selectedCount==1&&!selectedName.empty()&&parentName&&
                    std::wstring_view(parentName.get())==selectedName&&
                    bounds.right>bounds.left&&bounds.bottom>bounds.top&&parentBounds.top>=selectedBounds.bottom&&
                    bounds.left>=parentBounds.left&&bounds.top>=parentBounds.top&&
                    bounds.right<=parentBounds.right&&bounds.bottom<=parentBounds.bottom;
                if(current) {
                    ++groupCount;observed={selectedName,name?name.get():L"",bounds};
                }
            }
            if(FAILED(status))return status;
            if(groupCount>1)return E_UNEXPECTED;
            if(groupCount==1) {*result=std::move(observed);return S_OK;}
            Sleep(10);
        }while(GetTickCount64()<deadline);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    });
    if(SUCCEEDED(hr))output=std::move(*result);
    return hr;
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
    bool stock=false,iconOnly=false;
    for(int index=1;index<argc;++index) {
        const std::string_view argument(argv[index]);
        if(argument=="--installed")stock=true;
        if(argument=="--icon-cache-only")iconOnly=true;
    }
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
        bool labelCallbackRequested=false;
        HRESULT labelCallbackStatus=E_UNEXPECTED;
        std::wstring labelCallbackOutput=L"unchanged label output";
        HRESULT callbackInvalidation=E_UNEXPECTED,callbackFlush=S_OK;
        unsigned executions=0;UINT lastCommand=0;unsigned heightEvents=0;unsigned copyItemsQueries=0;unsigned itemExecutions=0;UINT lastItemCommand=0,lastItem=0;
        UINT librarySelectedIndex=UI_COLLECTION_INVALIDINDEX;
        bool replacementLibraryLocations=false;
        bool firstLibrarySourceRefresh=false;
        HRESULT firstLibraryInvalidation=E_UNEXPECTED,firstLibraryFlush=S_OK;
        explorer::RibbonCallbacks callbacks;
        callbacks.execute=[&](UINT id){++executions;lastCommand=id;return S_OK;};
        callbacks.executeItem=[&](UINT id,UINT index){++itemExecutions;lastItemCommand=id;lastItem=index;return S_OK;};
        callbacks.query=[&](UINT id){
            if(labelCallbackRequested&&id==explorer::Copy) {
                labelCallbackRequested=false;
                labelCallbackStatus=ribbon.commandLabel(explorer::RibbonHomeTab,labelCallbackOutput);
            }
            explorer::RibbonCommandState state;state.enabled=id==explorer::Paste?false:id!=explorer::Copy||copyEnabled;state.checked=id==explorer::HiddenItems;state.selectedIndex=id==explorer::RibbonLayoutGallery?5:id==explorer::LibraryDefault?librarySelectedIndex:UI_COLLECTION_INVALIDINDEX;return state;
        };
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
            if(id==explorer::LibraryDefault) {
                explorer::RibbonCollectionReadback demand;
                succeeded(ribbon.collectionReadback(id,demand),"Native Library source callback phase");
                if(!firstLibrarySourceRefresh&&demand.sourceRequests) {
                    firstLibrarySourceRefresh=true;
                    firstLibraryInvalidation=ribbon.invalidateItems(id);
                    firstLibraryFlush=ribbon.flush();
                }
                for(UINT index=0;index<3;++index)
                    items.push_back({index,std::wstring(replacementLibraryLocations?L"Owned replacement library location ":L"Owned library location ")+std::to_wstring(index),false,L"imageres.dll,-3"});
            }
            return items;
        };
        callbacks.heightChanged=[&](UINT){++heightEvents;};
        succeeded(ribbon.initialize(window.handle,GetModuleHandleW(nullptr),std::move(callbacks),stock?explorer::RibbonLayout::InstalledWindows10:explorer::RibbonLayout::Authored),"Native framework initialization");
        if(stock&&ribbon.layout()!=explorer::RibbonLayout::InstalledWindows10) {
            std::cout<<"SKIP: installed Windows 10 build19045 layout is unavailable, HRESULT="<<static_cast<ULONG>(ribbon.installedLayoutStatus())<<'\n';
            ribbon.reset();DestroyWindow(window.handle);window.handle=nullptr;OleUninitialize();return 77;
        }
        require(ribbon.valid()&&ribbon.height()>20&&heightEvents>0,"Native Ribbon view/height callback");
        require(!IsWindowVisible(window.handle),"Host was shown");
        nativeIconCacheEquivalence(ribbon,window.handle);
        if(iconOnly) {
            bool visibleInput=true;succeeded(desktop.visibleWindowsOnInputDesktop(visibleInput),"Icon-only input desktop window guard");
            require(!visibleInput,"Native icon comparison created a visible input desktop window");
            ribbon.reset();DestroyWindow(window.handle);window.handle=nullptr;OleUninitialize();return 0;
        }
        // Render solely on the non-input private desktop. This cannot surface
        // a window on the user's desktop and supplies normal native paint/layout.
        ShowWindow(window.handle,SW_SHOWNOACTIVATE);UpdateWindow(window.handle);pump();
        succeeded(ribbon.invalidateState(),"All state properties preserve compiled labels");succeeded(ribbon.flush(),"Flush state labels");pump();
        std::wstring tabLabel;succeeded(ribbon.commandLabel(explorer::RibbonHomeTab,tabLabel),"Home label survives state invalidation");
        std::vector<std::wstring> visibleLabels;
        if(stock) {
            InstalledLabelReference reference;reference.initialize(hostClass.lpszClassName,ribbon.features());
            // Group labels come directly from the installed BML even when its
            // Label callback supplies no currentValue. Compare all five actual
            // native Home toolbars with an independent unmodified framework;
            // an authored fallback such as Organize is not that provenance.
            ShowWindow(reference.window.handle,SW_SHOWNOACTIVATE);UpdateWindow(reference.window.handle);pump();
            std::vector<std::wstring> referenceGroups,actualGroups;
            succeeded(ownedHomeGroupLabels(reference.window.handle,referenceGroups),"Read independently realized native Home group captions");
            succeeded(ownedHomeGroupLabels(window.handle,actualGroups),"Read actual owned installed Home group captions");
            require(referenceGroups.size()==5&&actualGroups==referenceGroups,"Installed native Home group captions changed from their compiled BML");
            std::cout<<"PASS: all five installed Home group captions match independent native BML UIA:";
            for(const auto& caption:referenceGroups)std::cout<<" ["<<diagnosticCaption(caption)<<']';
            std::cout<<'\n';
            ULONG languageCount=0,languageCharacters=0;
            constexpr DWORD languageFlags=MUI_LANGUAGE_NAME|MUI_UI_FALLBACK;
            require(GetThreadPreferredUILanguages(languageFlags,&languageCount,nullptr,&languageCharacters)&&languageCharacters>1,
                "Read actual UI resource fallback languages");
            std::vector<wchar_t> languageNames(languageCharacters,L'\0');
            require(GetThreadPreferredUILanguages(languageFlags,&languageCount,languageNames.data(),&languageCharacters)&&languageCount,
                "Read actual UI resource language names");
            const auto effectiveLanguage=LocaleNameToLCID(languageNames.data(),LOCALE_ALLOW_NEUTRAL_NAMES);
            require(effectiveLanguage!=0,"Resolve actual preferred UI language");
            if(PRIMARYLANGID(effectiveLanguage)==LANG_ENGLISH) {
                for(const auto& expected:std::array{
                    std::pair{static_cast<UINT>(explorer::RibbonEasyAccessMenu),L"Easy access"},
                    std::pair{static_cast<UINT>(explorer::RibbonOptionsMenu),L"Options"},
                    std::pair{static_cast<UINT>(explorer::RibbonFolderOptions),L"Options"},
                    std::pair{static_cast<UINT>(explorer::RibbonAccessMedia),L"Access media"},
                    std::pair{1527U,L"Extract To"}}) {
                    std::wstring current;succeeded(ribbon.commandLabel(expected.first,current),"Read English stock presentation correction");
                    require(current==expected.second,"Existing English stock presentation correction was lost");
                }
            }
            UINT nativeResourceLabels=0;
            for(const UINT command:{static_cast<UINT>(explorer::RibbonHomeTab),static_cast<UINT>(explorer::RibbonShareTab),static_cast<UINT>(explorer::RibbonViewTab),1506U,1507U}) {
                const auto nativeCommand=ribbon.nativeCommandId(command);
                succeeded(reference.framework->InvalidateUICommand(nativeCommand,UI_INVALIDATIONS_PROPERTY,&UI_PKEY_Label),
                    "Request actual installed Label callback currentValue");
                succeeded(reference.framework->FlushPendingInvalidations(),"Materialize actual installed Label callback");
                require(reference.handler->labelRequests[nativeCommand]>0,"Installed reference did not reach its native Label callback");
                std::wstring current;succeeded(ribbon.commandLabel(command,current),"Read installed fallback label outside property callback");
                require(!current.empty(),"A missing installed resource blanked an existing host tab/group");
                if(const auto resource=reference.handler->labels.find(nativeCommand);resource!=reference.handler->labels.end()) {
                    ++nativeResourceLabels;
                    require(current==resource->second,"Native callback currentValue lost its installed text");
                }
                if(command!=1506U&&command!=1507U)visibleLabels.push_back(std::move(current));
            }
            labelCallbackRequested=true;
            succeeded(ribbon.invalidateState(explorer::Copy),"Request real callback-boundary label read");
            succeeded(ribbon.flush(),"Commit callback-boundary label request");pump();
            require(!labelCallbackRequested&&labelCallbackStatus==S_OK&&labelCallbackOutput==tabLabel,
                "Owned label snapshot could not be read without entering the framework from UpdateProperty");
            succeeded(ribbon.commandLabel(explorer::RibbonHomeTab,labelCallbackOutput),"Read native fallback after property callback returned");
            require(labelCallbackOutput==tabLabel,"Native fallback label changed across property callback boundary");
            std::cout<<"PASS: native Label callback boundary; installed nonempty resource labels="<<nativeResourceLabels
                <<"; explicit authored fallbacks="<<5-nativeResourceLabels<<'\n';
        } else require(tabLabel==L"Home","Authored Home resource label disappeared");
        for(const UINT command:{static_cast<UINT>(explorer::Copy),static_cast<UINT>(explorer::Cut),static_cast<UINT>(explorer::Properties),static_cast<UINT>(explorer::NewFolder)}) {
            explorer::NamespaceCommandMetadata native;
            succeeded(explorer::namespaceCommandMetadata(explorer::ribbonCommandStoreName(command),&native),"Independently read installed native command title");
            std::wstring current;succeeded(ribbon.commandLabel(command,current),"Read retained native-provider title");
            require(!native.label.empty()&&current==native.label,"Native provider title lost its localized text");
            visibleLabels.push_back(native.label);
        }
        std::cout<<"Owned label host: visible="<<IsWindowVisible(window.handle)<<" ribbonHeight="<<ribbon.height()<<'\n';
        succeeded(ownedAutomation(window.handle,[handle=window.handle,labels=visibleLabels](IUIAutomation* automation) {
            HRESULT firstFailure=S_OK;
            for(const auto& expected:labels) {
                Microsoft::WRL::ComPtr<IUIAutomationElement> control;
                auto status=namedElement(automation,handle,expected.c_str(),&control);
                if(SUCCEEDED(status)) {
                    BSTR actual=nullptr;status=control->get_CurrentName(&actual);
                    const bool exact=SUCCEEDED(status)&&actual&&std::wstring_view(actual)==expected;
                    SysFreeString(actual);if(SUCCEEDED(status)&&!exact)status=E_UNEXPECTED;
                }
                diagnoseOwnedLabel(automation,handle,expected,status);
                if(FAILED(status)&&SUCCEEDED(firstFailure))firstFailure=status;
            }
            if(FAILED(firstFailure))diagnoseOwnedRibbonTabs(automation,handle);
            return firstFailure;
        }),"Actual owned UIA retained every host/native-provider control name");
        std::wstring unchangedLabel=L"unchanged invalid label";
        require(ribbon.commandLabel(0xffffffffU,unchangedLabel)==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)&&unchangedLabel==L"unchanged invalid label",
            "Invalid label request changed output");
        if(stock) {
            InstalledLabelReference reference;reference.initialize(hostClass.lpszClassName,ribbon.features());
            const auto nativeContext=ribbon.nativeCommandId(explorer::RibbonSearchContext);
            const auto nativeKind=ribbon.nativeCommandId(explorer::SearchKindMenu);
            require(reference.handler->commandTypes.contains(nativeContext)&&
                reference.handler->commandTypes.at(nativeContext)==UI_COMMANDTYPE_CONTEXT,
                "Independent BML did not register the actual Search context");
            Variant active;succeeded(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_ACTIVE,&active.value),"Native Search reference context value");
            succeeded(reference.framework->SetUICommandProperty(nativeContext,UI_PKEY_ContextAvailable,active.value),
                "Activate independently realized native Search reference");
            ShowWindow(reference.window.handle,SW_SHOWNOACTIVATE);UpdateWindow(reference.window.handle);pump();
            Variant contextValue;
            const auto contextRead=reference.framework->GetUICommandProperty(nativeContext,UI_PKEY_ContextAvailable,&contextValue.value);
            ULONG contextState=UI_CONTEXTAVAILABILITY_NOTAVAILABLE;
            const auto contextStateRead=SUCCEEDED(contextRead)?PropVariantToUInt32(contextValue.value,&contextState):contextRead;
            const auto kindType=reference.handler->commandTypes.find(nativeKind);
            const auto kindFirstType=reference.handler->firstLabelTypes.find(nativeKind);
            const auto kindFirstLabel=reference.handler->firstLabels.find(nativeKind);
            std::cout<<"Owned independent Search context: nativeContext="<<nativeContext<<" read="<<static_cast<ULONG>(contextRead)
                <<" valueType="<<contextValue.value.vt<<" stateRead="<<static_cast<ULONG>(contextStateRead)<<" availability="<<contextState
                <<" nativeKind="<<nativeKind<<" kindRegistered="<<(kindType!=reference.handler->commandTypes.end())
                <<" kindType="<<(kindType==reference.handler->commandTypes.end()?UI_COMMANDTYPE_UNKNOWN:kindType->second)
                <<" kindLabelRequests="<<reference.handler->labelRequests[nativeKind]<<" firstCurrentType="
                <<(kindFirstType==reference.handler->firstLabelTypes.end()?static_cast<VARTYPE>(VT_EMPTY):kindFirstType->second)
                <<" firstCurrent=\""<<(kindFirstLabel==reference.handler->firstLabels.end()?"<absent>":diagnosticCaption(kindFirstLabel->second))<<"\"\n";
            succeeded(contextStateRead,"Read actual independent Search contextual availability");
            require(contextState==UI_CONTEXTAVAILABILITY_ACTIVE,"Independent Search context is not natively active");
            OwnedRefineLabels nativeLabels,actualLabels;
            const auto referenceRead=ownedSearchRefineLabels(reference.window.handle,nativeLabels);
            diagnoseRefineLabels("independent BML",nativeLabels);
            succeeded(referenceRead,"Read independent native Search Refine parent captions");
            require(reference.handler->commandTypes.contains(nativeKind)&&
                reference.handler->commandTypes.at(nativeKind)==UI_COMMANDTYPE_COLLECTION,
                "Independent BML did not register realized Kind as a native collection");
            succeeded(reference.framework->InvalidateUICommand(nativeKind,UI_INVALIDATIONS_PROPERTY,&UI_PKEY_Label),
                "Request independent native Kind callback value");
            succeeded(reference.framework->FlushPendingInvalidations(),"Materialize independent native Kind label");
            succeeded(ribbon.setContexts(explorer::RibbonContext::Search,true),"Activate actual production Search label fixture");
            succeeded(ribbon.flush(),"Materialize actual production Search labels");pump();
            const auto actualRead=ownedSearchRefineLabels(window.handle,actualLabels);
            diagnoseRefineLabels("production",actualLabels);
            std::wstring retained;succeeded(ribbon.commandLabel(explorer::SearchKindMenu,retained),"Read retained production Kind label outside callback");
            const auto first=reference.handler->firstLabels.find(nativeKind);
            const auto firstType=reference.handler->firstLabelTypes.find(nativeKind);
            std::cout<<"Owned native Kind label: nativeCommand="<<nativeKind<<" commandType="<<reference.handler->commandTypes.at(nativeKind)
                <<" requests="<<reference.handler->labelRequests[nativeKind]<<" firstCurrentType="
                <<(firstType==reference.handler->firstLabelTypes.end()?static_cast<VARTYPE>(VT_EMPTY):firstType->second)
                <<" firstCurrent=\""<<(first==reference.handler->firstLabels.end()?"<absent>":diagnosticCaption(first->second))
                <<"\" production=\""<<diagnosticCaption(retained)<<"\" referenceRead="<<static_cast<ULONG>(referenceRead)
                <<" actualRead="<<static_cast<ULONG>(actualRead)<<'\n';
            succeeded(actualRead,"Read actual production Search Refine parent captions");
            require(reference.handler->labelRequests[nativeKind]>0,"Independent native Kind Label callback was never reached");
            require(nativeLabels.selectedTab==actualLabels.selectedTab&&nativeLabels.toolbar==actualLabels.toolbar&&
                nativeLabels.parentType==actualLabels.parentType&&nativeLabels.groupLabels.size()==4&&
                nativeLabels.groupLabels==actualLabels.groupLabels,
                "Production Search group captions differ from independent native BML, including its empty Close group");
            require(nativeLabels.controls.size()==4&&actualLabels.controls.size()==4,
                "Native Refine whole-gallery structure changed");
            for(size_t index=0;index<nativeLabels.controls.size();++index)
                require(nativeLabels.controls[index].type==UIA_SplitButtonControlTypeId&&
                    actualLabels.controls[index].type==nativeLabels.controls[index].type,
                    "Native Refine whole-gallery control type changed");
            bool bmlKindAvailable=false;
            const auto requireNativeKind=[&](std::wstring_view caption) {
                if(caption.empty())return;
                bmlKindAvailable=true;
                require(retained==caption&&actualLabels.controls[1].name==caption,
                    "Production Kind label replaced an independently observed installed BML caption");
            };
            if(first!=reference.handler->firstLabels.end())requireNativeKind(first->second);
            if(const auto observed=reference.handler->labels.find(nativeKind);observed!=reference.handler->labels.end())
                requireNativeKind(observed->second);
            requireNativeKind(nativeLabels.controls[1].name);
            if(!bmlKindAvailable) {
                explorer::NamespaceCommandMetadata provider;
                const auto providerRead=explorer::namespaceCommandMetadata(explorer::ribbonCommandStoreName(explorer::SearchKindMenu),&provider);
                std::cout<<"Owned Kind caption provenance: BMLUnavailable firstCurrentType="
                    <<(firstType==reference.handler->firstLabelTypes.end()?static_cast<VARTYPE>(VT_EMPTY):firstType->second)
                    <<" providerRead="<<static_cast<ULONG>(providerRead)<<" providerTitle=\""<<diagnosticCaption(provider.label)
                    <<"\" actualParentUiName=\""<<diagnosticCaption(actualLabels.controls[1].name)<<"\" KindCaptionParity=false\n";
                succeeded(providerRead,"Independently read native Kind CommandStore/provider title");
                require(!provider.label.empty()&&retained==provider.label&&actualLabels.controls[1].name==provider.label,
                    "Actual Kind parent lost its independently read native provider title");
            } else std::cout<<"Owned Kind caption provenance: BMLAvailable KindCaptionParity=true\n";
            std::cout<<"PASS: all four actual Search group captions match independent native BML, including empty captions\n";
            explorer::NamespaceCommandMetadata closeProvider;
            succeeded(explorer::namespaceCommandMetadata(explorer::ribbonCommandStoreName(explorer::CloseSearch),&closeProvider),
                "Independently read Close search leaf title");
            require(!closeProvider.label.empty(),"Native Close search leaf title is unavailable");
            succeeded(ownedAutomation(window.handle,[handle=window.handle,label=closeProvider.label](IUIAutomation* automation) {
                Microsoft::WRL::ComPtr<IUIAutomationElement> leaf;
                auto status=namedElement(automation,handle,label.c_str(),&leaf);
                CONTROLTYPEID type=0;BOOL offscreen=TRUE;RECT bounds{};
                if(SUCCEEDED(status))status=leaf->get_CurrentControlType(&type);
                if(SUCCEEDED(status))status=leaf->get_CurrentIsOffscreen(&offscreen);
                if(SUCCEEDED(status))status=leaf->get_CurrentBoundingRectangle(&bounds);
                if(SUCCEEDED(status)&&(type!=UIA_ButtonControlTypeId||offscreen||bounds.right<=bounds.left||bounds.bottom<=bounds.top))
                    status=E_UNEXPECTED;
                return status;
            }),"Empty native Close group preserves its actual visible Close search leaf title");
            succeeded(ribbon.setContexts(explorer::RibbonContext::None),"Independent Search label fixture cleanup");
            succeeded(ribbon.selectTab(explorer::RibbonHomeTab),"Restore Home after independent Search label fixture");pump();
        }
        if(stock) {
            InstalledLabelReference reference;reference.initialize(hostClass.lpszClassName,ribbon.features());
            ShowWindow(reference.window.handle,SW_SHOWNOACTIVATE);UpdateWindow(reference.window.handle);pump();
            struct VideoVariant {UINT context,group;};
            constexpr std::array videoVariants{VideoVariant{0x702,0x2c20},VideoVariant{0x70a,0x2c21}};
            std::array<OwnedSingleGroupLabel,videoVariants.size()> nativeVideoGroups;
            for(size_t index=0;index<videoVariants.size();++index) {
                const auto variant=videoVariants[index];
                require(reference.handler->commandTypes.contains(variant.context)&&
                    reference.handler->commandTypes.at(variant.context)==UI_COMMANDTYPE_CONTEXT,
                    "Independent BML did not register the requested Video context variant");
                Variant active;succeeded(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_ACTIVE,&active.value),
                    "Native Video reference availability value");
                succeeded(reference.framework->SetUICommandProperty(variant.context,UI_PKEY_ContextAvailable,active.value),
                    "Activate unmodified native Video context variant");
                succeeded(reference.framework->FlushPendingInvalidations(),"Realize independent Video BML");pump();
                require(reference.handler->commandTypes.contains(variant.group)&&
                    reference.handler->commandTypes.at(variant.group)==UI_COMMANDTYPE_GROUP,
                    "Independent BML did not realize the exact Video group command");
                succeeded(reference.framework->InvalidateUICommand(variant.group,UI_INVALIDATIONS_PROPERTY,&UI_PKEY_Label),
                    "Request independent native Video group label callback");
                succeeded(reference.framework->FlushPendingInvalidations(),"Commit independent Video group label read");pump();
                Variant property;
                const auto propertyRead=reference.framework->GetUICommandProperty(variant.group,UI_PKEY_Label,&property.value);
                succeeded(ownedSingleGroupLabel(reference.window.handle,nativeVideoGroups[index]),
                    "Read real selected Video content group's native UIA Name property");
                const auto firstType=reference.handler->firstLabelTypes.find(variant.group);
                const auto firstLabel=reference.handler->firstLabels.find(variant.group);
                std::cout<<"Owned independent Video group: context="<<variant.context<<" nativeGroup="<<variant.group
                    <<" propertyRead="<<static_cast<ULONG>(propertyRead)<<" propertyType="<<property.value.vt
                    <<" labelRequests="<<reference.handler->labelRequests[variant.group]<<" firstCurrentType="
                    <<(firstType==reference.handler->firstLabelTypes.end()?static_cast<VARTYPE>(VT_EMPTY):firstType->second)
                    <<" firstCurrent=\""<<(firstLabel==reference.handler->firstLabels.end()?"<absent>":diagnosticCaption(firstLabel->second))
                    <<"\" selectedTab=\""<<diagnosticCaption(nativeVideoGroups[index].selectedTab)
                    <<"\" actualUiCaption=\""<<diagnosticCaption(nativeVideoGroups[index].caption)
                    <<"\" bounds="<<nativeVideoGroups[index].bounds.left<<','<<nativeVideoGroups[index].bounds.top<<','
                    <<nativeVideoGroups[index].bounds.right<<','<<nativeVideoGroups[index].bounds.bottom<<'\n';
                require(reference.handler->labelRequests[variant.group]>0,
                    "Independent native Video group Label callback was never reached");
                if(SUCCEEDED(propertyRead))require(property.value.vt==VT_LPWSTR&&
                    std::wstring_view(property.value.pwszVal?property.value.pwszVal:L"")==nativeVideoGroups[index].caption,
                    "Independent native Video UIProperty and actual UIA group caption disagree");
                Variant unavailable;
                succeeded(InitPropVariantFromUInt32(UI_CONTEXTAVAILABILITY_NOTAVAILABLE,&unavailable.value),
                    "Native Video reference cleanup value");
                succeeded(reference.framework->SetUICommandProperty(variant.context,UI_PKEY_ContextAvailable,unavailable.value),
                    "Clear the independently tested native Video context variant");pump();
            }
            const size_t selectedVariant=ribbon.features().mediaFoundation?0:1;
            const auto expectedVariant=videoVariants[selectedVariant];
            succeeded(ribbon.setContexts(explorer::RibbonContext::Video,true),"Activate actual feature-selected Video production context");
            succeeded(ribbon.flush(),"Materialize actual production Video group caption");pump();
            UINT nativeContext=0,availability=0;
            succeeded(ribbon.contextAvailable(explorer::RibbonContext::Video,nativeContext,availability),
                "Read production Video context variant from native framework");
            require(nativeContext==expectedVariant.context&&availability==UI_CONTEXTAVAILABILITY_ACTIVE,
                "Production Video group did not use the actual native feature-selected variant");
            OwnedSingleGroupLabel actualVideoGroup;
            succeeded(ownedSingleGroupLabel(window.handle,actualVideoGroup),"Read actual production Video group's visible native UIA Name");
            Variant actualProperty;
            const auto actualPropertyRead=ribbon.nativeFramework()->GetUICommandProperty(expectedVariant.group,UI_PKEY_Label,&actualProperty.value);
            std::cout<<"Owned production Video group: nativeContext="<<nativeContext<<" nativeGroup="<<expectedVariant.group
                <<" propertyRead="<<static_cast<ULONG>(actualPropertyRead)<<" propertyType="<<actualProperty.value.vt
                <<" selectedTab=\""<<diagnosticCaption(actualVideoGroup.selectedTab)<<"\" actualUiCaption=\""
                <<diagnosticCaption(actualVideoGroup.caption)<<"\" referenceUiCaption=\""
                <<diagnosticCaption(nativeVideoGroups[selectedVariant].caption)<<"\"\n";
            require(actualVideoGroup.selectedTab==nativeVideoGroups[selectedVariant].selectedTab&&
                actualVideoGroup.caption==nativeVideoGroups[selectedVariant].caption,
                "Production Video group caption differs from independently loaded native BML, including an empty caption");
            succeeded(ribbon.setContexts(explorer::RibbonContext::None),"Independent Video label fixture cleanup");
            succeeded(ribbon.selectTab(explorer::RibbonHomeTab),"Restore Home after independent Video label fixture");pump();
            std::cout<<"PASS: actual feature-selected Video group caption matches independent native BML\n";
        }
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
            succeeded(ribbon.setContexts(explorer::RibbonContext::Library,true),"Select native Library item gallery");pump();
            std::wstring libraryLabel;succeeded(ribbon.commandLabel(explorer::LibraryDefault,libraryLabel),"Native Library save-location label");
            succeeded(expandOwnedGallery(window.handle,libraryLabel.c_str()),"Expand actual Library item gallery");pump();
            Variant librarySource;
            succeeded(ribbon.nativeFramework()->GetUICommandProperty(ribbon.nativeCommandId(explorer::LibraryDefault),
                UI_PKEY_ItemsSource,&librarySource.value),"Actual materialized Library source");
            Microsoft::WRL::ComPtr<IUICollection> libraryCollection;UINT libraryCount=0;
            require(librarySource.value.vt==VT_UNKNOWN&&librarySource.value.punkVal,"Native Library source type");
            succeeded(librarySource.value.punkVal->QueryInterface(IID_PPV_ARGS(&libraryCollection)),"Native Library source collection");
            succeeded(libraryCollection->GetCount(&libraryCount),"Actual materialized Library row count");
            require(libraryCount==3,"Native Library fixture rows were not materialized");
            require(firstLibrarySourceRefresh&&firstLibraryInvalidation==S_OK&&firstLibraryFlush==E_PENDING,
                "Initial Library callback did not exercise its owner refresh boundary");
            for(UINT row=0;row<libraryCount;++row) {
                const auto label=L"Owned library location "+std::to_wstring(row);
                succeeded(checkOwnedGalleryRow(window.handle,label.c_str(),true,false),"First native Library popup kept every location after owner refresh");
            }
            // Initial Home has no Library item collection. Publish a valid
            // mock index only after the real three-row source has materialized;
            // no native property query runs from inside a property callback.
            librarySelectedIndex=2;
            succeeded(ribbon.invalidateState(explorer::LibraryDefault),"Publish actual Library fixture selection");
            succeeded(ribbon.flush(),"Materialized Library selection flush");pump();
            const auto selectedLibrary=[&] {
                Variant selected;ULONG index=UI_COLLECTION_INVALIDINDEX;
                succeeded(ribbon.nativeFramework()->GetUICommandProperty(ribbon.nativeCommandId(explorer::LibraryDefault),UI_PKEY_SelectedItem,&selected.value),"Native selected Library location readback");
                succeeded(PropVariantToUInt32(selected.value,&index),"Native selected Library location type");return index;
            };
            require(selectedLibrary()==2,"First Library source materialization lost the selected location");
            librarySelectedIndex=3;
            succeeded(ribbon.invalidateState(explorer::LibraryDefault),"Reject out-of-range Library selection");
            succeeded(ribbon.flush(),"Out-of-range Library selection flush");pump();
            require(selectedLibrary()==UI_COLLECTION_INVALIDINDEX,"Native gallery received an out-of-range selection");
            librarySelectedIndex=2;
            succeeded(ribbon.invalidateState(explorer::LibraryDefault),"Restore valid Library selection");
            succeeded(ribbon.flush(),"Valid Library selection flush");pump();
            require(selectedLibrary()==2,"Valid Library selection was not restored");
            succeeded(expandOwnedGallery(window.handle,libraryLabel.c_str()),"Present retained native Library popup");pump();
            explorer::RibbonCollectionReadback libraryBeforeReplacement;
            succeeded(ribbon.collectionReadback(explorer::LibraryDefault,libraryBeforeReplacement),"Retained Library popup source generation");
            const auto verifyLibraryRows=[&](bool replacement,bool visible=true) {
                UINT actualCount=0;succeeded(libraryCollection->GetCount(&actualCount),"Actual replacement Library row count");
                require(actualCount==3,"Library source replacement lost a location");
                for(UINT row=0;row<actualCount;++row) {
                    Microsoft::WRL::ComPtr<IUnknown> raw;Microsoft::WRL::ComPtr<IUISimplePropertySet> properties;
                    succeeded(libraryCollection->GetItem(row,&raw),"Actual replacement Library item");
                    succeeded(raw.As(&properties),"Actual replacement Library properties");
                    Variant title;succeeded(properties->GetValue(UI_PKEY_Label,&title.value),"Actual replacement Library title");
                    const auto expected=std::wstring(replacement?L"Owned replacement library location ":L"Owned library location ")+std::to_wstring(row);
                    require(title.value.vt==VT_LPWSTR&&title.value.pwszVal&&expected==title.value.pwszVal,"Library source retained a stale title");
                    if(visible)succeeded(checkOwnedGalleryRow(window.handle,expected.c_str(),true,false),"Actual visible native Library location row");
                }
            };
            verifyLibraryRows(false);
            librarySelectedIndex=1;
            replacementLibraryLocations=true;
            succeeded(ribbon.invalidateItems(explorer::LibraryDefault),"Refresh changed Library locations");
            const auto libraryDiagnostic=[&](const char* phase) {
                explorer::RibbonCollectionReadback read;
                succeeded(ribbon.collectionReadback(explorer::LibraryDefault,read),"Library selected callback diagnostics");
                std::cout<<"Native Library "<<phase<<": sources="<<read.sourceRequests<<" selectedRequests="<<read.selectedRequests
                    <<" lastSelectedIndex="<<read.lastSelectedIndex<<" publishedItems="<<read.publishedItems
                    <<" pendingInvalidations="<<read.pendingInvalidations<<std::endl;
            };
            libraryDiagnostic("before-source-flush");
            succeeded(ribbon.flush(),"Changed Library source flush");libraryDiagnostic("after-source-flush");pump();
            libraryDiagnostic("after-owner-dispatch");
            explorer::RibbonCollectionReadback openLibrarySnapshot;
            succeeded(ribbon.collectionReadback(explorer::LibraryDefault,openLibrarySnapshot),"Open native Library popup snapshot");
            require(openLibrarySnapshot.sourceRequests==libraryBeforeReplacement.sourceRequests&&selectedLibrary()==2,
                "The open native Library popup did not retain its original source snapshot");
            ExpandCollapseState libraryExpansion=ExpandCollapseState_LeafNode;
            succeeded(galleryExpansionState(window.handle,libraryLabel.c_str(),libraryExpansion),"Actual Library popup state after source invalidation");
            std::cout<<"Native Library expansion after source invalidation="<<libraryExpansion<<std::endl;
            verifyLibraryRows(false,false);
            succeeded(galleryExpansionState(window.handle,libraryLabel.c_str(),libraryExpansion,true),"Close retained native Library popup");pump();
            require(libraryExpansion==ExpandCollapseState_Collapsed,"Retained Library popup did not close");
            succeeded(expandOwnedGallery(window.handle,libraryLabel.c_str()),"Reopen actual native Library source");pump();
            succeeded(ribbon.flush(),"Commit actual replacement Library value");pump();
            libraryDiagnostic("after-native-source-reopen");
            explorer::RibbonCollectionReadback reloadedLibrarySnapshot;
            succeeded(ribbon.collectionReadback(explorer::LibraryDefault,reloadedLibrarySnapshot),"Reloaded native Library popup source");
            require(reloadedLibrarySnapshot.sourceRequests>libraryBeforeReplacement.sourceRequests,
                "Reopening Library locations did not reload their actual native source");
            verifyLibraryRows(true);
            std::cout<<"Native replaced Library selected index="<<selectedLibrary()<<std::endl;
            require(selectedLibrary()==1,"Replacing Library locations lost their selected index");
            succeeded(galleryExpansionState(window.handle,libraryLabel.c_str(),libraryExpansion,true),"Collapse native Library item gallery");
            require(libraryExpansion==ExpandCollapseState_Collapsed,"Library gallery remained expanded");
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
        for (UINT tab = explorer::RibbonHomeTab; tab <= explorer::RibbonShortcutTab; ++tab) {
            const std::array<UINT,1> structural{tab};
            require(ribbon.setQuickAccessCommands(structural)==E_INVALIDARG,"Structural QAT tab accepted");
        }
        for (UINT context = explorer::RibbonPictureContext; context <= explorer::RibbonShortcutContext; ++context) {
            const std::array<UINT,1> structural{context};
            require(ribbon.setQuickAccessCommands(structural)==E_INVALIDARG,"Structural QAT context accepted");
        }
        succeeded(ribbon.quickAccessCommands(qat),"QAT after rejected structural commands");
        require(qat==std::vector<UINT>(custom.begin(),custom.end()),"Rejected structural commands changed the QAT");
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
        require(ribbon.saveSettings({})==E_INVALIDARG&&ribbon.saveSettings(L"relative-ribbon-settings.bin")==E_INVALIDARG,
            "Native Ribbon settings accepted an empty or relative destination");
        succeeded(ribbon.saveSettings(settings),"Atomic Unicode native settings save");
        succeeded(ribbon.setMinimized(false),"Native expand");succeeded(ribbon.setQuickAccessBelow(false),"QAT restore top");
        const std::array<UINT,1> reset{explorer::NewFolder};succeeded(ribbon.setQuickAccessCommands(reset),"QAT reset");
        succeeded(ribbon.loadSettings(settings),"Native settings restore");pump();
        succeeded(ribbon.minimized(minimized),"Restored collapse");succeeded(ribbon.quickAccessBelow(below),"Restored QAT placement");succeeded(ribbon.quickAccessCommands(qat),"Restored QAT commands");
        if(!(minimized&&below&&qat==std::vector<UINT>(custom.begin(),custom.end())))std::cerr<<"Settings readback minimized="<<minimized<<" below="<<below<<" commands="<<qat.size()<<'\n';
        require(minimized&&below&&qat==std::vector<UINT>(custom.begin(),custom.end()),"Native persisted state lost");
        const auto selectPersistedState=[&](bool replacement)->HRESULT {
            auto status=ribbon.setMinimized(!replacement);
            if(SUCCEEDED(status))status=ribbon.setQuickAccessBelow(!replacement);
            if(SUCCEEDED(status))status=replacement?ribbon.setQuickAccessCommands(reset):ribbon.setQuickAccessCommands(custom);
            return status;
        };
        explorer::test::stateFileSecurityProfiles(temporary,L"native-ribbon",
            [&](const std::filesystem::path& path,bool replacement)->HRESULT {
                const auto status=selectPersistedState(replacement);
                return SUCCEEDED(status)?ribbon.saveSettings(path):status;
            },
            [&](const std::filesystem::path& path,bool replacement) {
                succeeded(selectPersistedState(!replacement),"Disturb actual native state before security readback");
                succeeded(ribbon.loadSettings(path),"Load actual native codec under preserved security profile");
                succeeded(ribbon.minimized(minimized),"Security profile collapse readback");
                succeeded(ribbon.quickAccessBelow(below),"Security profile QAT placement readback");
                succeeded(ribbon.quickAccessCommands(qat),"Security profile QAT command readback");
                const auto expected=replacement?std::vector<UINT>(reset.begin(),reset.end()):std::vector<UINT>(custom.begin(),custom.end());
                require(minimized==!replacement&&below==!replacement&&qat==expected,
                    "Actual native Ribbon codec changed complete state under a security profile");
            });
        succeeded(ribbon.loadSettings(settings),"Restore original native state after security profiles");
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
