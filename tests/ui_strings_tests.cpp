#include "explorer/ui_strings.hpp"
#include <shlwapi.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void succeeded(HRESULT status,const char* message) {
    if(FAILED(status))throw std::runtime_error(std::string(message)+" HRESULT="+std::to_string(static_cast<ULONG>(status)));
}
bool auditedBuild() {
    using VersionFunction=LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto version=reinterpret_cast<VersionFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
    OSVERSIONINFOW info{sizeof(info)};
    return version&&version(&info)>=0&&info.dwMajorVersion==10&&info.dwBuildNumber==19045;
}
std::wstring systemModule(const wchar_t* name) {
    std::array<wchar_t,32768> directory{};
    const auto count=GetSystemDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
    require(count&&count<directory.size(),"Cannot read actual system resource directory");
    return std::wstring(directory.data(),count)+L"\\"+name;
}
std::wstring independentResource(const std::wstring& module,UINT resource) {
    const auto source=L"@"+module+L",-"+std::to_wstring(resource);
    std::array<wchar_t,8192> value{};
    succeeded(SHLoadIndirectString(source.c_str(),value.data(),static_cast<UINT>(value.size()),nullptr),
        "Independent public MUI indirect-string lookup");
    require(*value.data()!=L'\0',"Independent native UI string was empty");
    return value.data();
}
std::vector<std::wstring> actualLanguages() {
    constexpr DWORD flags=MUI_LANGUAGE_NAME|MUI_UI_FALLBACK;
    ULONG count=0,characters=0;
    require(GetThreadPreferredUILanguages(flags,&count,nullptr,&characters)&&count&&characters>1,
        "Cannot read actual thread resource fallback languages");
    std::wstring buffer(characters,L'\0');
    require(GetThreadPreferredUILanguages(flags,&count,buffer.data(),&characters)!=FALSE,
        "Cannot materialize actual thread resource fallback languages");
    std::vector<std::wstring> result;
    for(size_t start=0;start+1<buffer.size()&&buffer[start];) {
        const auto end=buffer.find(L'\0',start);
        require(end!=buffer.npos,"Native language list lacked a terminator");
        result.emplace_back(buffer.substr(start,end-start));start=end+1;
    }
    require(result.size()==count,"Native language count differed from actual names");
    return result;
}
void realInstalledResources() {
    using explorer::UiText;
    struct Case { UiText key; const wchar_t* module; UINT resource; };
    const std::array cases{
        Case{UiText::Back,L"shell32.dll",49858},Case{UiText::Forward,L"shell32.dll",49859},
        Case{UiText::BackTooltip,L"ExplorerFrame.dll",14348},Case{UiText::ForwardTooltip,L"ExplorerFrame.dll",14349},
        Case{UiText::UpTooltip,L"ExplorerFrame.dll",14343},Case{UiText::RecentLocations,L"ExplorerFrame.dll",14337},
        Case{UiText::AddressBar,L"ExplorerFrame.dll",12352},Case{UiText::AddressName,L"ExplorerFrame.dll",13829},
        Case{UiText::SearchBox,L"ExplorerFrame.dll",34304},Case{UiText::SearchAction,L"ExplorerFrame.dll",34305},
        Case{UiText::SearchCue,L"ExplorerFrame.dll",13830},Case{UiText::RefreshTooltip,L"ExplorerFrame.dll",12662},
        Case{UiText::NavigationButtons,L"ExplorerFrame.dll",34817},Case{UiText::AddressToolbar,L"ExplorerFrame.dll",34816},
        Case{UiText::AllLocations,L"ExplorerFrame.dll",34818},Case{UiText::MinimiseRibbon,L"uiribbon.dll",6104},
        Case{UiText::MinimiseRibbonTooltip,L"uiribbon.dll",6100},Case{UiText::ExpandRibbonTooltip,L"uiribbon.dll",6101},
        Case{UiText::PreviewPaneName,L"shell32.dll",38228},Case{UiText::PreviewSelectFile,L"shell32.dll",38245},
        Case{UiText::PreviewUnavailable,L"shell32.dll",38246}};
    const auto languages=actualLanguages();const auto audited=auditedBuild();
    for(const auto& test:cases) {
        explorer::UiString value;succeeded(explorer::loadUiString(test.key,&value),"Load named host UI string");
        require(!value.text.empty()&&value.uiLanguages==languages,"Host label lost actual MUI language provenance");
        if(audited) {
            const auto module=systemModule(test.module);
            require(value.provenance==explorer::UiStringProvenance::Native&&value.nativeStatus==S_OK&&
                value.module==module&&value.resourceId==test.resource,"Audited installed label became an authored fallback");
            require(value.text==independentResource(module,test.resource),"Host label differs from actual independently resolved MUI text");
        }else require(value.provenance==explorer::UiStringProvenance::AuthorFallback&&FAILED(value.nativeStatus)&&
            value.module.empty()&&!value.resourceId,"An unaudited Windows build claimed verified resource provenance");
    }
    // There is no audited standalone native title for these two controls.
    // Do not derive a title by trimming an arbitrary translated shortcut.
    for(const auto& test:std::array{std::pair{UiText::Up,L"Up"},std::pair{UiText::ExpandRibbon,L"Expand the Ribbon"}}) {
        explorer::UiString value;succeeded(explorer::loadUiString(test.first,&value),"Load explicit authored-only title");
        require(value.text==test.second&&value.provenance==explorer::UiStringProvenance::AuthorFallback&&
            FAILED(value.nativeStatus)&&value.module.empty()&&!value.resourceId,"Missing native plain title was fabricated");
    }
    explorer::UiString refresh;succeeded(explorer::loadUiString(UiText::Refresh,&refresh),"Load native address-action Refresh title");
    if(audited) {
        const auto strip=independentResource(systemModule(L"ExplorerFrame.dll"),12656);
        const auto first=strip.find(L'|',1),second=first==strip.npos?strip.npos:strip.find(L'|',first+1);
        const auto third=second==strip.npos?strip.npos:strip.find(L'|',second+1);
        require(!strip.empty()&&strip.front()==L'|'&&first!=strip.npos&&second!=strip.npos&&third!=strip.npos,
            "Actual address-action strip lacked its three native controls");
        require(refresh.provenance==explorer::UiStringProvenance::Native&&refresh.resourceId==12656&&
            refresh.text==strip.substr(second+1,third-second-1),"Refresh title differs from the native address-action control slot");
    }else require(refresh.provenance==explorer::UiStringProvenance::AuthorFallback&&refresh.text==L"Refresh",
        "Unaudited Refresh title lost its authored fallback");
}
void literalTemplateInsertion() {
    using explorer::UiTemplateSlot;
    const std::wstring folder=L"Owned 日本語 \U0001f5c2 100% %s %1 %10 %n %999 ! and &";
    std::wstring result;
    succeeded(explorer::replaceUiStringTemplate(L"Search %1",UiTemplateSlot::MessageInsert,folder,&result),"Insert literal owned Unicode search folder");
    require(result==L"Search "+folder,"Search folder percent signs became template instructions");
    succeeded(explorer::replaceUiStringTemplate(L"Address: %s",UiTemplateSlot::StringInsert,folder,&result),"Insert literal owned Unicode address");
    require(result==L"Address: "+folder,"Address percent signs became format instructions");
    succeeded(explorer::replaceUiStringTemplate(L"%1",UiTemplateSlot::MessageInsert,L"",&result),"Insert empty owned replacement");
    require(result.empty(),"Empty replacement left a fabricated placeholder");
    for(const auto key:{explorer::UiText::SearchCue,explorer::UiText::AddressName,explorer::UiText::RefreshTooltip}) {
        explorer::UiString pattern,formatted;
        succeeded(explorer::loadUiString(key,&pattern),"Read actual native or authored template");
        succeeded(explorer::formatUiString(key,folder,&formatted),"Format actual native UI template literally");
        const auto marker=key==explorer::UiText::SearchCue?L"%1":L"%s";
        const auto position=pattern.text.find(marker);
        require(position!=pattern.text.npos&&formatted.text==pattern.text.substr(0,position)+folder+pattern.text.substr(position+2),
            "Native localized template did not preserve exact prefix/folder/suffix");
        require(formatted.provenance==pattern.provenance&&formatted.nativeStatus==pattern.nativeStatus&&
            formatted.module==pattern.module&&formatted.resourceId==pattern.resourceId&&formatted.uiLanguages==pattern.uiLanguages,
            "Template insertion changed native resource provenance");
    }
    const std::wstring maximum(32767,L'界');
    succeeded(explorer::replaceUiStringTemplate(L"%1",UiTemplateSlot::MessageInsert,maximum,&result),"Preserve maximum native path-sized replacement");
    require(result==maximum,"Long native replacement was truncated");
    const auto largestPattern=std::wstring(4094,L'x')+L"%1";
    succeeded(explorer::replaceUiStringTemplate(largestPattern,UiTemplateSlot::MessageInsert,folder,&result),"Preserve bounded maximum template");
    require(result==std::wstring(4094,L'x')+folder,"Maximum template lost its literal text");
}
void malformedTemplatesAndOutput() {
    using explorer::UiTemplateSlot;
    for(const auto slot:{UiTemplateSlot::MessageInsert,UiTemplateSlot::StringInsert}) {
        std::vector<std::wstring> patterns{L"",L"No slot",L"%",L"%2",L"%n",L"%%",L"%1 %s",L"%1 %1",L"%s %s",L"%1!s!",L"%10",L"%hs",L"%02s",
            std::wstring(L"x\0%1",5),std::wstring(1,static_cast<wchar_t>(0xd800))+L"%1",std::wstring(4095,L'x')+L"%1"};
        patterns.push_back(slot==UiTemplateSlot::MessageInsert?L"%s":L"%1");
        for(const auto& pattern:patterns) {
            std::wstring unchanged=L"unchanged";
            require(explorer::replaceUiStringTemplate(pattern,slot,L"owned",&unchanged)==E_INVALIDARG&&unchanged==L"unchanged",
                "Malformed template changed output or became a formatting program");
        }
    }
    for(const auto& replacement:{std::wstring(L"x\0y",3),std::wstring(1,static_cast<wchar_t>(0xd800)),
        std::wstring(1,static_cast<wchar_t>(0xdc00)),std::wstring(32768,L'x')}) {
        std::wstring unchanged=L"unchanged";
        require(explorer::replaceUiStringTemplate(L"%1",UiTemplateSlot::MessageInsert,replacement,&unchanged)==E_INVALIDARG&&unchanged==L"unchanged",
            "Malformed replacement changed output");
        explorer::UiString value;value.text=L"unchanged";value.nativeStatus=E_ABORT;
        require(explorer::formatUiString(explorer::UiText::SearchCue,replacement,&value)==E_INVALIDARG&&
            value.text==L"unchanged"&&value.nativeStatus==E_ABORT,"Malformed localized insertion changed output provenance");
    }
    std::wstring unchanged=L"unchanged";
    require(explorer::replaceUiStringTemplate(L"%1",static_cast<UiTemplateSlot>(-1),L"owned",&unchanged)==E_INVALIDARG&&unchanged==L"unchanged",
        "Invalid slot type changed output");
    require(explorer::replaceUiStringTemplate(L"%1",UiTemplateSlot::MessageInsert,L"owned",nullptr)==E_POINTER,"Null template output accepted");
    explorer::UiString value;value.text=L"unchanged";value.nativeStatus=E_ABORT;
    require(explorer::loadUiString(static_cast<explorer::UiText>(-1),&value)==E_INVALIDARG&&value.text==L"unchanged"&&value.nativeStatus==E_ABORT,
        "Invalid UI label key changed output");
    require(explorer::formatUiString(explorer::UiText::Back,L"owned",&value)==E_INVALIDARG&&value.text==L"unchanged",
        "A non-template UI label accepted formatting");
    require(explorer::loadUiString(explorer::UiText::Back,nullptr)==E_POINTER&&
        explorer::formatUiString(explorer::UiText::SearchCue,L"owned",nullptr)==E_POINTER,"Null UI result accepted");
}
void languageAwareThreadCaches() {
    explorer::UiString initial;succeeded(explorer::loadUiString(explorer::UiText::SearchCue,&initial),"Read initializing thread native template");
    const auto languages=actualLanguages();
    bool workerSucceeded=false;
    std::thread worker([&] {
        try {
            const auto workerLanguages=actualLanguages();
            for(unsigned iteration=0;iteration<32;++iteration) {
                explorer::UiString value;
                succeeded(explorer::loadUiString(explorer::UiText::SearchCue,&value),"Read worker native template");
                require(value.uiLanguages==workerLanguages,"Worker UI label reused another thread's language provenance");
                if(workerLanguages==languages)require(value.text==initial.text&&value.provenance==initial.provenance&&
                    value.module==initial.module&&value.resourceId==initial.resourceId,"Equal native language contexts returned unstable text");
            }
            workerSucceeded=true;
        }catch(const std::exception& error){std::cerr<<"UI string worker: "<<error.what()<<'\n';}
    });
    worker.join();require(workerSucceeded,"Read-only thread cache failed");
    explorer::UiString after;succeeded(explorer::loadUiString(explorer::UiText::SearchCue,&after),"Re-read initializing thread template");
    require(after.text==initial.text&&after.uiLanguages==languages,"Worker changed initializing thread UI language/text");
}
} // namespace

int runUiStringsTests() {
    unsigned failures=0;
    const std::array tests{
        std::pair<const char*,void(*)()>{"installed resource text and fallback provenance",realInstalledResources},
        std::pair<const char*,void(*)()>{"native templates preserve literal Unicode/percent values",literalTemplateInsertion},
        std::pair<const char*,void(*)()>{"malformed templates and output preservation",malformedTemplatesAndOutput},
        std::pair<const char*,void(*)()>{"actual MUI language contexts and independent thread caches",languageAwareThreadCaches}};
    for(const auto& [name,test]:tests) {
        try{test();std::cout<<"PASS: Host UI strings: "<<name<<'\n';}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL: Host UI strings: "<<name<<": "<<error.what()<<'\n';}
        catch(...){++failures;std::cerr<<"FAIL: Host UI strings: "<<name<<": unknown exception\n";}
    }
    return static_cast<int>(failures);
}
