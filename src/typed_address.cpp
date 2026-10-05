#include "explorer/typed_address.hpp"
#include <shellapi.h>
#include <array>
#include <new>

namespace explorer {
namespace {
bool space(wchar_t value) {return value==L' '||value==L'\t'||value==L'\r'||value==L'\n';}
std::wstring_view leftTrim(std::wstring_view value) {
    while(!value.empty()&&space(value.front()))value.remove_prefix(1);
    return value;
}
std::wstring_view rightTrim(std::wstring_view value) {
    while(!value.empty()&&space(value.back()))value.remove_suffix(1);
    return value;
}
bool protocol(std::wstring_view input) {
    const auto colon=input.find(L':');
    // A single-letter prefix is a Windows drive, rather than a URI scheme.
    if(colon==input.npos||colon<=1)return false;
    for(size_t index=0;index<colon;++index) {
        const auto ch=input[index];const bool alpha=(ch>=L'A'&&ch<=L'Z')||(ch>=L'a'&&ch<=L'z');
        if(!alpha&&(index==0||!((ch>=L'0'&&ch<=L'9')||ch==L'+'||ch==L'-'||ch==L'.')))return false;
    }
    return true;
}
size_t executableEnd(std::wstring_view input) {
    constexpr std::array suffixes{L".exe",L".com",L".bat",L".cmd"};
    for(size_t index=0;index+4<=input.size();++index) {
        if(input[index]!=L'.')continue;
        for(const auto* suffix:suffixes) {
            if(CompareStringOrdinal(input.data()+index,4,suffix,4,TRUE)==CSTR_EQUAL&&
               (index+4==input.size()||space(input[index+4])))return index+4;
        }
    }
    return input.npos;
}
bool validText(std::wstring_view input) {
    return input.size()<=32767&&input.find(L'\0')==input.npos&&
        (input.empty()||WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input.data(),
            static_cast<int>(input.size()),nullptr,0,nullptr,nullptr)!=0);
}
}
HRESULT parseTypedAddressLaunch(std::wstring_view input,TypedAddressLaunch* result) {
    if(!result)return E_POINTER;
    if(input.empty()||!validText(input))return E_INVALIDARG;
    input=leftTrim(input);if(input.empty())return E_INVALIDARG;
    try {
        TypedAddressLaunch parsed;
        if(protocol(input))parsed.target=rightTrim(input);
        else if(input.front()==L'"') {
            const auto end=input.find(L'"',1);
            if(end==input.npos||end==1||(end+1<input.size()&&!space(input[end+1])))return E_INVALIDARG;
            parsed.target=input.substr(1,end-1);
            parsed.parameters=leftTrim(input.substr(end+1));
        } else {
            const auto firstSpace=input.find_first_of(L" \t\r\n");
            const auto firstToken=input.substr(0,firstSpace);
            // A command alias ends at its first separator. Looking for an
            // executable suffix in its arguments would turn `cmd /c a.exe`
            // into one nonexistent executable. Unquoted paths can still
            // contain spaces; filenames containing spaces without a path
            // prefix must be quoted to distinguish them from parameters.
            const bool path=firstToken.find_first_of(L"\\/:")!=firstToken.npos;
            auto end=executableEnd(path?input:firstToken);
            if(end==input.npos)end=firstSpace;
            if(end==input.npos)end=input.size();
            parsed.target=input.substr(0,end);
            parsed.parameters=leftTrim(input.substr(end));
        }
        if(parsed.target.empty()||parsed.target.find(L'"')!=parsed.target.npos)return E_INVALIDARG;
        *result=std::move(parsed);return S_OK;
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
      catch(...) {return E_FAIL;}
}
HRESULT launchTypedAddress(HWND owner,std::wstring_view input,bool headless,const TypedAddressLauncher& testLauncher,std::wstring_view directory) {
    if(headless)return E_ACCESSDENIED;
    if(!validText(directory))return E_INVALIDARG;
    TypedAddressLaunch launch;auto hr=parseTypedAddressLaunch(input,&launch);
    if(FAILED(hr))return hr;
    try{launch.directory=directory;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
    if(testLauncher) {
        try{return testLauncher(owner,launch);}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
    }
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.hwnd=owner;execute.fMask=SEE_MASK_FLAG_NO_UI|SEE_MASK_DOENVSUBST;
    execute.lpVerb=L"open";execute.lpFile=launch.target.c_str();
    execute.lpDirectory=launch.directory.empty()?nullptr:launch.directory.c_str();
    execute.lpParameters=launch.parameters.empty()?nullptr:launch.parameters.c_str();execute.nShow=SW_SHOWNORMAL;
    return ShellExecuteExW(&execute)?S_OK:HRESULT_FROM_WIN32(GetLastError());
}
}
