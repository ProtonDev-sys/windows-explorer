#include "explorer/typed_address.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void splitting() {
    using explorer::TypedAddressLaunch;
    struct Case {const wchar_t* input;const wchar_t* target;const wchar_t* parameters;};
    constexpr Case cases[]{
        {L"notepad",L"notepad",L""},{L"cmd /c \"echo a & b\"  ",L"cmd",L"/c \"echo a & b\"  "},
        {L"  \"C:\\Owned Folder\\日本語.exe\"  /x \"quoted value\" ",L"C:\\Owned Folder\\日本語.exe",L"/x \"quoted value\" "},
        {L"C:\\Owned Folder\\Tool.EXE /arg:\"two words\"",L"C:\\Owned Folder\\Tool.EXE",L"/arg:\"two words\""},
        {L"%WINDIR%\\system32\\cmd.exe /k",L"%WINDIR%\\system32\\cmd.exe",L"/k"},
        {L"cmd /c C:\\Owned\\tool.exe",L"cmd",L"/c C:\\Owned\\tool.exe"},
        {L"notepad \"C:\\Owned Folder\\report.exe\"",L"notepad",L"\"C:\\Owned Folder\\report.exe\""},
        {L"tool -source owned.bat",L"tool",L"-source owned.bat"},
        {L"cmd.exe /c owned.cmd",L"cmd.exe",L"/c owned.cmd"},
        {L".\\Owned Folder\\tool.exe /arg",L".\\Owned Folder\\tool.exe",L"/arg"},
        {L"\"Owned Tool.exe\" /arg",L"Owned Tool.exe",L"/arg"},
        {L"https://example.invalid/owned?q=%20value",L"https://example.invalid/owned?q=%20value",L""},
        {L"ms-settings:display",L"ms-settings:display",L""},
        {L"mailto:owned@example.invalid?subject=two words",L"mailto:owned@example.invalid?subject=two words",L""},
        {L"\\\\OwnedHost\\Owned Share\\tool.cmd /d \"x\\\"\"",L"\\\\OwnedHost\\Owned Share\\tool.cmd",L"/d \"x\\\"\""}};
    for(const auto& test:cases) {
        TypedAddressLaunch parsed;
        require(explorer::parseTypedAddressLaunch(test.input,&parsed)==S_OK&&parsed.target==test.target&&parsed.parameters==test.parameters,
            "Typed executable or URL lost original parameter spelling");
    }
}
void invalidInput() {
    explorer::TypedAddressLaunch unchanged{L"unchanged",L"parameters"};
    const std::array<std::wstring,7> invalid{L"",L" \t",L"\"\"",L"\"unterminated",L"\"tool\"suffix",std::wstring(L"x\0y",3),std::wstring(1,static_cast<wchar_t>(0xd800))};
    for(const auto& value:invalid)require(explorer::parseTypedAddressLaunch(value,&unchanged)==E_INVALIDARG&&unchanged.target==L"unchanged"&&unchanged.parameters==L"parameters",
        "Malformed typed input changed output or became executable");
    require(explorer::parseTypedAddressLaunch(L"cmd",nullptr)==E_POINTER,"Null result accepted");
}
void routing() {
    unsigned calls=0;
    const explorer::TypedAddressLauncher mock=[&](HWND owner,const explorer::TypedAddressLaunch& value) {
        ++calls;require(owner==reinterpret_cast<HWND>(static_cast<UINT_PTR>(7))&&value.target==L"cmd"&&value.parameters==L"/c \"owned only\""&&value.directory==L"C:\\Owned Unicode 日本語",
            "Owned mock dispatch did not retain owner or arguments");return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    };
    require(explorer::launchTypedAddress(nullptr,L"cmd",true,mock)==E_ACCESSDENIED&&calls==0,"Headless input reached a launcher");
    require(explorer::launchTypedAddress(nullptr,L"\"unterminated",true,mock)==E_ACCESSDENIED&&calls==0,"Malformed headless input bypassed guard");
    require(explorer::launchTypedAddress(nullptr,L"\"unterminated",false,mock)==E_INVALIDARG&&calls==0,"Malformed normal input reached mock");
    require(explorer::launchTypedAddress(reinterpret_cast<HWND>(static_cast<UINT_PTR>(7)),L"cmd /c \"owned only\"",false,mock,L"C:\\Owned Unicode 日本語")==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&calls==1,
        "Explicit mock dispatch changed native result");
    for(const auto& invalid: {std::wstring(L"x\0y",3),std::wstring(1,static_cast<wchar_t>(0xd800)),
                             std::wstring(1,static_cast<wchar_t>(0xdc00)),std::wstring(32768,L'a')})
        require(explorer::launchTypedAddress(nullptr,L"cmd",false,mock,invalid)==E_INVALIDARG&&calls==1,
            "Malformed working directory reached a launcher");
}
}
int runTypedAddressTests() {
    unsigned failures=0;
    for(const auto& [name,test]:std::array<std::pair<const char*,void(*)()>,3>{{{"exact executable and URI splitting",splitting},{"malformed input output preservation",invalidInput},{"headless denial and owned mock routing",routing}}}) {
        try{test();std::cout<<"PASS: Typed address: "<<name<<'\n';}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL: Typed address: "<<name<<": "<<error.what()<<'\n';}
    }
    return static_cast<int>(failures);
}
