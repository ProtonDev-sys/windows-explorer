#include "explorer/ui_strings.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <utility>

namespace explorer {
namespace {
constexpr size_t maximumResourceCharacters = 4096;
constexpr size_t maximumReplacementCharacters = 32767;
enum class Module : size_t { ExplorerFrame, Shell, Ribbon, Count, None };
enum class Shape { Plain, MessageTemplate, StringTemplate, AddressActionStrip, FallbackOnly };
struct Entry { Module module; UINT resource; Shape shape; const wchar_t* fallback; };
constexpr std::array<Entry, static_cast<size_t>(UiText::Count)> catalogue{{
    {Module::Shell,49858,Shape::Plain,L"Back"},
    {Module::Shell,49859,Shape::Plain,L"Forward"},
    {Module::ExplorerFrame,14348,Shape::Plain,L"Back (Alt+Left Arrow)"},
    {Module::ExplorerFrame,14349,Shape::Plain,L"Forward (Alt+Right Arrow)"},
    {Module::None,0,Shape::FallbackOnly,L"Up"},
    {Module::ExplorerFrame,14343,Shape::Plain,L"Up one level (Alt + Up Arrow)"},
    {Module::ExplorerFrame,14337,Shape::Plain,L"Recent locations"},
    {Module::ExplorerFrame,12352,Shape::Plain,L"Address Bar"},
    {Module::ExplorerFrame,13829,Shape::StringTemplate,L"Address: %s"},
    {Module::ExplorerFrame,34304,Shape::Plain,L"Search Box"},
    {Module::ExplorerFrame,34305,Shape::Plain,L"Search"},
    {Module::ExplorerFrame,13830,Shape::MessageTemplate,L"Search %1"},
    {Module::ExplorerFrame,12656,Shape::AddressActionStrip,L"Refresh"},
    {Module::ExplorerFrame,12662,Shape::StringTemplate,L"Refresh \"%s\" (F5)"},
    {Module::ExplorerFrame,34817,Shape::Plain,L"Navigation buttons"},
    {Module::ExplorerFrame,34816,Shape::Plain,L"Address band toolbar"},
    {Module::ExplorerFrame,34818,Shape::Plain,L"All locations"},
    {Module::Ribbon,6104,Shape::Plain,L"Minimize the Ribbon"},
    {Module::Ribbon,6100,Shape::Plain,L"Minimize the Ribbon (Ctrl+F1)"},
    {Module::None,0,Shape::FallbackOnly,L"Expand the Ribbon"},
    {Module::Ribbon,6101,Shape::Plain,L"Expand the Ribbon (Ctrl+F1)"}
}};
constexpr std::array<const wchar_t*,static_cast<size_t>(Module::Count)> moduleNames{
    L"ExplorerFrame.dll",L"shell32.dll",L"uiribbon.dll"};

bool validText(std::wstring_view value, size_t maximum, bool singleLine) noexcept {
    if(value.size()>maximum)return false;
    for(size_t index=0;index<value.size();++index) {
        const auto ch=static_cast<unsigned>(value[index]);
        if(!ch||(singleLine&&ch<32))return false;
        if(ch>=0xd800&&ch<=0xdbff) {
            if(++index==value.size())return false;
            const auto low=static_cast<unsigned>(value[index]);
            if(low<0xdc00||low>0xdfff)return false;
        }else if(ch>=0xdc00&&ch<=0xdfff)return false;
    }
    return true;
}
bool templatePosition(std::wstring_view pattern, UiTemplateSlot slot, size_t& position) noexcept {
    if(pattern.empty()||!validText(pattern,maximumResourceCharacters,true))return false;
    wchar_t marker=0;
    switch(slot) {
    case UiTemplateSlot::MessageInsert:marker=L'1';break;
    case UiTemplateSlot::StringInsert:marker=L's';break;
    default:return false;
    }
    size_t found=pattern.npos;
    for(size_t index=0;index<pattern.size();++index)if(pattern[index]==L'%') {
        if(found!=pattern.npos||index+1==pattern.size()||pattern[index+1]!=marker)return false;
        found=index;++index;
        // %10 and %1!s! are different FormatMessage expressions, not our one
        // audited %1 slot. A printf length/width suffix is likewise unsupported.
        if(marker==L'1'&&index+1<pattern.size()&&
           ((pattern[index+1]>=L'0'&&pattern[index+1]<=L'9')||pattern[index+1]==L'!'))return false;
    }
    if(found==pattern.npos)return false;
    position=found;return true;
}
bool shapeText(const Entry& entry,std::wstring& text) {
    if(text.empty()||!validText(text,maximumResourceCharacters,true))return false;
    size_t ignored=0;
    switch(entry.shape) {
    case Shape::MessageTemplate:return templatePosition(text,UiTemplateSlot::MessageInsert,ignored);
    case Shape::StringTemplate:return templatePosition(text,UiTemplateSlot::StringInsert,ignored);
    case Shape::Plain:return text.find(L'%')==text.npos;
    case Shape::AddressActionStrip: {
        // Audited 19045 address-action order: Go, Stop, Refresh, empty fourth
        // cell. All three localized labels must be present and contain no
        // formatting/extra delimiters; never search arbitrary DLL text.
        std::array<std::wstring_view,6> fields{};
        size_t start=0,count=0;
        const std::wstring_view strip(text);
        for(size_t index=0;index<=strip.size();++index)if(index==strip.size()||strip[index]==L'|') {
            if(count==fields.size())return false;
            fields[count++]=strip.substr(start,index-start);start=index+1;
        }
        if(count!=fields.size()||!fields[0].empty()||!fields[4].empty()||!fields[5].empty())return false;
        for(size_t index=1;index<=3;++index)
            if(fields[index].empty()||fields[index].find(L'%')!=fields[index].npos)return false;
        if(fields[3].front()==L' '||fields[3].back()==L' ')return false;
        text=std::wstring(fields[3]);return true;
    }
    default:return false;
    }
}
HRESULT lastErrorResult(DWORD fallback) noexcept {
    const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:fallback);
}
bool auditedBuild() noexcept {
    using VersionFunction=LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll=GetModuleHandleW(L"ntdll.dll");
    const auto version=ntdll?reinterpret_cast<VersionFunction>(GetProcAddress(ntdll,"RtlGetVersion")):nullptr;
    OSVERSIONINFOW info{sizeof(info)};
    return version&&version(&info)>=0&&info.dwMajorVersion==10&&info.dwBuildNumber==19045;
}
struct LanguageKey {
    LANGID threadLanguage=0;
    std::wstring fingerprint;
    std::vector<std::wstring> names;
};
HRESULT readLanguages(LanguageKey& result) {
    constexpr DWORD flags=MUI_LANGUAGE_NAME|MUI_UI_FALLBACK;
    ULONG count=0,characters=0;
    if(!GetThreadPreferredUILanguages(flags,&count,nullptr,&characters))return lastErrorResult(ERROR_INVALID_DATA);
    if(!count||characters<2||characters>32768)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::wstring buffer(characters,L'\0');
    if(!GetThreadPreferredUILanguages(flags,&count,buffer.data(),&characters))return lastErrorResult(ERROR_INVALID_DATA);
    if(characters>buffer.size()||characters<2||buffer[characters-1]||buffer[characters-2])return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    buffer.resize(characters);
    LanguageKey candidate;candidate.threadLanguage=GetThreadUILanguage();candidate.fingerprint=buffer;
    size_t start=0;
    while(start+1<buffer.size()&&buffer[start]) {
        const auto end=buffer.find(L'\0',start);
        if(end==buffer.npos||end==start||end-start>=LOCALE_NAME_MAX_LENGTH)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        candidate.names.emplace_back(buffer.substr(start,end-start));start=end+1;
    }
    if(candidate.names.size()!=count)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    result=std::move(candidate);return S_OK;
}
struct ResourceModule {
    HMODULE handle=nullptr;
    std::wstring path;
    HRESULT status=E_PENDING;
    ResourceModule()=default;
    ResourceModule(const ResourceModule&)=delete;
    ResourceModule& operator=(const ResourceModule&)=delete;
    ~ResourceModule(){if(handle)FreeLibrary(handle);}
    void reset() noexcept {if(handle)FreeLibrary(handle);handle=nullptr;path.clear();status=E_PENDING;}
};
class StringCache {
public:
    HRESULT load(UiText key,UiString& output) {
        const auto index=static_cast<size_t>(key);
        LanguageKey current;
        const auto languageStatus=readLanguages(current);
        if(FAILED(languageStatus)) {
            output=fallback(index,languageStatus,{});return S_OK;
        }
        if(language_.threadLanguage!=current.threadLanguage||language_.fingerprint!=current.fingerprint) {
            for(auto& module:modules_)module.reset();
            for(auto& value:strings_)value.reset();
            familyStatus_=E_PENDING;language_=std::move(current);
        }
        if(!strings_[index]) {
            const auto& entry=catalogue[index];
            auto value=fallback(index,HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),language_.names);
            if(entry.shape==Shape::FallbackOnly)value.nativeStatus=HRESULT_FROM_WIN32(ERROR_RESOURCE_NAME_NOT_FOUND);
            else if(audited_) {
                const auto family=validateFamily();
                value.nativeStatus=family;
                if(SUCCEEDED(family)) {
                    auto& module=loadModule(entry.module);
                    value.nativeStatus=module.status;
                    if(SUCCEEDED(module.status)) {
                        const wchar_t* pointer=nullptr;
                        SetLastError(ERROR_SUCCESS);
                        const auto characters=LoadStringW(module.handle,entry.resource,reinterpret_cast<LPWSTR>(&pointer),0);
                        if(characters<=0||!pointer)value.nativeStatus=lastErrorResult(ERROR_RESOURCE_NAME_NOT_FOUND);
                        else if(static_cast<size_t>(characters)>maximumResourceCharacters)value.nativeStatus=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                        else {
                            std::wstring text(pointer,static_cast<size_t>(characters));
                            if(!shapeText(entry,text))value.nativeStatus=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                            else {
                                value.text=std::move(text);value.provenance=UiStringProvenance::Native;
                                value.nativeStatus=S_OK;value.module=module.path;value.resourceId=entry.resource;
                            }
                        }
                    }
                }
            }
            strings_[index]=std::move(value);
        }
        output=*strings_[index];return S_OK;
    }
private:
    static UiString fallback(size_t index,HRESULT status,const std::vector<std::wstring>& languages) {
        UiString result;result.text=catalogue[index].fallback;result.nativeStatus=status;result.uiLanguages=languages;return result;
    }
    ResourceModule& loadModule(Module which) {
        auto& result=modules_[static_cast<size_t>(which)];
        if(result.status!=E_PENDING)return result;
        std::array<wchar_t,32768> directory{};
        SetLastError(ERROR_SUCCESS);
        const auto characters=GetSystemDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
        if(!characters||characters>=directory.size()) {
            result.status=lastErrorResult(ERROR_INSUFFICIENT_BUFFER);return result;
        }
        result.path.assign(directory.data(),characters);result.path+=L'\\';result.path+=moduleNames[static_cast<size_t>(which)];
        SetLastError(ERROR_SUCCESS);
        result.handle=LoadLibraryExW(result.path.c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        result.status=result.handle?S_OK:lastErrorResult(ERROR_MOD_NOT_FOUND);return result;
    }
    HRESULT validateFamily() {
        if(familyStatus_!=E_PENDING)return familyStatus_;
        auto& frame=loadModule(Module::ExplorerFrame);
        if(FAILED(frame.status))return familyStatus_=frame.status;
        const auto resource=FindResourceW(frame.handle,L"EXPLORER_RIBBON",L"UIFILE");
        const auto size=resource?SizeofResource(frame.handle,resource):0;
        const auto loaded=resource?LoadResource(frame.handle,resource):nullptr;
        const auto bytes=loaded?static_cast<const BYTE*>(LockResource(loaded)):nullptr;
        // Identify the audited native resource family without pinning a DLL
        // patch version, MUI language, or exact BML byte count.
        if(!bytes||size<15||size>1024*1024||memcmp(bytes+9,"SCBin$",6))
            return familyStatus_=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        for(const std::wstring_view symbol:{L"cmdTabHome",L"cmdChunkOrganize",L"cmdQAT"}) {
            const auto begin=reinterpret_cast<const BYTE*>(symbol.data());
            if(std::search(bytes,bytes+size,begin,begin+symbol.size()*sizeof(wchar_t))==bytes+size)
                return familyStatus_=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        return familyStatus_=S_OK;
    }
    bool audited_=auditedBuild();
    LanguageKey language_;
    std::array<ResourceModule,static_cast<size_t>(Module::Count)> modules_;
    std::array<std::optional<UiString>,catalogue.size()> strings_;
    HRESULT familyStatus_=E_PENDING;
};
StringCache& cache(){thread_local StringCache value;return value;}
const Entry* entryFor(UiText key) noexcept {
    const auto index=static_cast<size_t>(key);return index<catalogue.size()?&catalogue[index]:nullptr;
}
} // namespace

HRESULT replaceUiStringTemplate(std::wstring_view pattern,UiTemplateSlot slot,
    std::wstring_view replacement,std::wstring* output) noexcept {
    if(!output)return E_POINTER;
    size_t position=0;
    if(!templatePosition(pattern,slot,position)||!validText(replacement,maximumReplacementCharacters,false))return E_INVALIDARG;
    try {
        std::wstring candidate;candidate.reserve(pattern.size()-2+replacement.size());
        candidate.append(pattern.substr(0,position));candidate.append(replacement);candidate.append(pattern.substr(position+2));
        *output=std::move(candidate);return S_OK;
    }catch(...){return E_OUTOFMEMORY;}
}
HRESULT loadUiString(UiText key,UiString* output) noexcept {
    if(!output)return E_POINTER;
    if(!entryFor(key))return E_INVALIDARG;
    try {UiString candidate;const auto hr=cache().load(key,candidate);if(SUCCEEDED(hr))*output=std::move(candidate);return hr;}
    catch(...){return E_OUTOFMEMORY;}
}
HRESULT formatUiString(UiText key,std::wstring_view replacement,UiString* output) noexcept {
    if(!output)return E_POINTER;
    const auto entry=entryFor(key);
    if(!entry||(entry->shape!=Shape::MessageTemplate&&entry->shape!=Shape::StringTemplate))return E_INVALIDARG;
    try {
        UiString candidate;auto hr=loadUiString(key,&candidate);if(FAILED(hr))return hr;
        std::wstring formatted;
        hr=replaceUiStringTemplate(candidate.text,entry->shape==Shape::MessageTemplate?UiTemplateSlot::MessageInsert:UiTemplateSlot::StringInsert,replacement,&formatted);
        if(FAILED(hr))return hr;
        candidate.text=std::move(formatted);*output=std::move(candidate);return S_OK;
    }catch(...){return E_OUTOFMEMORY;}
}
} // namespace explorer
