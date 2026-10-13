#pragma once
// Admission is supplied by the separate, explicit, fresh GitHub-hosted job.
// This executable never creates a marker and exposes no local mutation mode.
#include <aclapi.h>
#include <sddl.h>
#include <filesystem>
#include <map>
#include <string>
#include <cstring>

namespace {
struct NativeHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    NativeHandle()=default;
    explicit NativeHandle(HANDLE handle):value(handle){}
    NativeHandle(const NativeHandle&)=delete;
    NativeHandle& operator=(const NativeHandle&)=delete;
    ~NativeHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
std::wstring environment(const wchar_t* name) {
    wchar_t text[4096]{};
    const auto length=GetEnvironmentVariableW(name,text,static_cast<DWORD>(std::size(text)));
    require(length>0&&length<std::size(text),"Required fresh Hosted admission environment is absent or oversized");
    return {text,length};
}
std::wstring currentSid() {
    NativeHandle token;
    require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)!=FALSE,"Read Hosted process token");
    DWORD length=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&length);
    require(length>0&&length<65536,"Bound Hosted token identity");
    std::vector<BYTE> bytes(length);
    require(GetTokenInformation(token.value,TokenUser,bytes.data(),length,&length)!=FALSE,"Read exact Hosted user SID");
    LPWSTR raw=nullptr;
    const auto converted=ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&raw);
    struct Sid {LPWSTR value;~Sid(){LocalFree(value);}} owned{raw};
    require(converted!=FALSE&&raw,"Format Hosted user SID");return std::wstring(raw);
}
FILE_ID_INFO nativeIdentity(HANDLE handle,bool directory) {
    FILE_ID_INFO id{};FILE_ATTRIBUTE_TAG_INFO tag{};
    require(GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id))!=FALSE,"Read full volume/128-bit native file identity");
    require(GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&tag,sizeof(tag))!=FALSE&&
        !(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&((tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory,
        "Native admission or owned target changed object type or became a reparse");
    return id;
}
bool sameNativeIdentity(const FILE_ID_INFO& left,const FILE_ID_INFO& right) {
    return left.VolumeSerialNumber==right.VolumeSerialNumber&&
        std::memcmp(left.FileId.Identifier,right.FileId.Identifier,16)==0;
}
std::wstring filesystemName(IShellItem* item) {
    PWSTR raw=nullptr;const auto result=item->GetDisplayName(SIGDN_FILESYSPATH,&raw);
    struct Release {PWSTR value;~Release(){CoTaskMemFree(value);}} release{raw};
    succeeded(result,"Read actual Shell filesystem identity");
    require(raw&&*raw,"Native filesystem path is empty");return raw;
}
struct HostedAdmission {
    NativeHandle marker,temporaryRoot,lease;
    FILE_ID_INFO markerIdentity{},rootIdentity{};
    std::filesystem::path markerPath,temporaryPath;
    std::wstring sid,nonce,caseMode;
    bool consumed=false;
    HostedAdmission(const std::filesystem::path& path,const std::wstring& requestedCase):caseMode(requestedCase) {
        require(environment(L"CI")==L"true"&&environment(L"GITHUB_ACTIONS")==L"true"&&
            environment(L"RUNNER_ENVIRONMENT")==L"github-hosted"&&
            environment(L"GITHUB_JOB")==L"app_pin_persistence_owned"&&
            environment(L"EXPLORER_HOSTED_PIN_MUTATION")==L"owned-guid-only",
            "Mutation requires the explicit disposable GitHub-hosted job; local/self-hosted mode is rejected");
        require((caseMode==L"normal-flow"||caseMode==L"final-destroy-only")&&caseMode==environment(L"EXPLORER_HOSTED_PIN_CASE"),
            "Fresh Hosted marker case differs from the explicitly selected CI case");
        nonce=environment(L"EXPLORER_HOSTED_PIN_NONCE");
        require(nonce.size()==36&&nonce.find_first_not_of(L"0123456789abcdefABCDEF-")==std::wstring::npos&&
            nonce[8]==L'-'&&nonce[13]==L'-'&&nonce[18]==L'-'&&nonce[23]==L'-',"Hosted nonce has invalid GUID spelling");
        temporaryPath=environment(L"RUNNER_TEMP");markerPath=path;
        require(temporaryPath.is_absolute()&&markerPath.is_absolute()&&
            markerPath.parent_path().lexically_normal()==temporaryPath.lexically_normal()&&
            markerPath.filename()==std::filesystem::path(L"ExplorerHostedPinsAdmission-"+nonce+L".txt")&&
            markerPath==std::filesystem::path(environment(L"EXPLORER_HOSTED_PIN_ADMISSION")),
            "Admission marker is not the exact job-issued RUNNER_TEMP marker");
        temporaryRoot.value=CreateFileW(temporaryPath.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(temporaryRoot.value!=INVALID_HANDLE_VALUE,"Retain Hosted temporary root");rootIdentity=nativeIdentity(temporaryRoot.value,true);
        marker.value=CreateFileW(markerPath.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(marker.value!=INVALID_HANDLE_VALUE,"Retain job-issued admission marker without write/delete sharing");
        markerIdentity=nativeIdentity(marker.value,false);
        sid=currentSid();PSID owner=nullptr;PSECURITY_DESCRIPTOR security=nullptr;
        const auto ownerRead=GetSecurityInfo(marker.value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&security);
        struct Security {PSECURITY_DESCRIPTOR value;~Security(){LocalFree(value);}} ownedSecurity{security};
        require(ownerRead==ERROR_SUCCESS&&owner,"Read actual marker owner");
        LPWSTR ownerText=nullptr;const auto converted=ConvertSidToStringSidW(owner,&ownerText);
        struct Sid {LPWSTR value;~Sid(){LocalFree(value);}} ownedSid{ownerText};
        require(converted!=FALSE&&ownerText,"Read marker owner SID");
        require(std::wstring(ownerText)==sid,"Admission marker belongs to a different user");
        LARGE_INTEGER size{};require(GetFileSizeEx(marker.value,&size)!=FALSE&&size.QuadPart>0&&size.QuadPart<8192,"Bound marker bytes");
        std::string bytes(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;
        require(ReadFile(marker.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE&&read==bytes.size(),"Read exact protected marker bytes");
        const auto wideSize=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
        require(wideSize>0,"Admission marker is not valid UTF-8");std::wstring text(static_cast<size_t>(wideSize),L'\0');
        require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),text.data(),wideSize)==wideSize,"Decode complete marker");
        std::map<std::wstring,std::wstring> fields;
        for(size_t at=0;at<text.size();) {
            const auto end=text.find(L'\n',at);require(end!=std::wstring::npos,"Admission marker lacks final newline");
            auto line=text.substr(at,end-at);if(!line.empty()&&line.back()==L'\r')line.pop_back();
            const auto equals=line.find(L'=');require(equals>0&&equals!=std::wstring::npos,"Malformed admission field");
            require(fields.emplace(line.substr(0,equals),line.substr(equals+1)).second,"Duplicate admission field");at=end+1;
        }
        require(fields.size()==15,"Admission marker schema differs from the approved job");
        const auto exact=[&](const wchar_t* key,const std::wstring& expected){require(fields.at(key)==expected,"Admission marker identity differs from the actual fresh job");};
        exact(L"version",L"1");exact(L"authorization",L"owned-quick-access-pin-roundtrip");
        exact(L"case",caseMode);
        exact(L"disposable",L"fresh-vm-single-job");exact(L"runner_environment",environment(L"RUNNER_ENVIRONMENT"));
        exact(L"job",environment(L"GITHUB_JOB"));exact(L"run_id",environment(L"GITHUB_RUN_ID"));
        exact(L"run_attempt",environment(L"GITHUB_RUN_ATTEMPT"));exact(L"runner_name",environment(L"RUNNER_NAME"));
        wchar_t machine[MAX_COMPUTERNAME_LENGTH+1]{};DWORD machineLength=static_cast<DWORD>(std::size(machine));
        require(GetComputerNameW(machine,&machineLength)!=FALSE,"Read actual fresh VM identity");exact(L"computer",machine);
        exact(L"sid",sid);exact(L"profile",environment(L"USERPROFILE"));exact(L"nonce",nonce);exact(L"source_sha",environment(L"GITHUB_SHA"));
        const auto& tickText=fields.at(L"marker_tick");require(!tickText.empty()&&tickText.find_first_not_of(L"0123456789")==std::wstring::npos,"Invalid marker monotonic time");
        const auto tick=std::stoull(tickText);const auto now=GetTickCount64();require(now>=tick&&now-tick<=120000,"Marker is stale or from a different VM boot");
        FILE_BASIC_INFO basic{};require(GetFileInformationByHandleEx(marker.value,FileBasicInfo,&basic,sizeof(basic))!=FALSE,"Read native marker creation time");
        FILETIME clock{};GetSystemTimeAsFileTime(&clock);ULARGE_INTEGER utc{};utc.LowPart=clock.dwLowDateTime;utc.HighPart=clock.dwHighDateTime;
        require(basic.CreationTime.QuadPart>0&&utc.QuadPart>=static_cast<ULONGLONG>(basic.CreationTime.QuadPart)&&
            utc.QuadPart-static_cast<ULONGLONG>(basic.CreationTime.QuadPart)<=1200000000ULL,"Marker creation is not fresh");
        const auto leasePath=markerPath.native()+L".consumed";
        lease.value=CreateFileW(leasePath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(lease.value!=INVALID_HANDLE_VALUE,"Fresh Hosted mutation marker was already consumed or cannot be leased");
        const std::string receipt="consumed owned-guid-only by PID="+std::to_string(GetCurrentProcessId())+"\n";DWORD written=0;
        require(WriteFile(lease.value,receipt.data(),static_cast<DWORD>(receipt.size()),&written,nullptr)!=FALSE&&written==receipt.size()&&FlushFileBuffers(lease.value)!=FALSE,
            "Write actual one-use admission receipt");consumed=true;verify();
    }
    void verify() const {
        require(consumed&&sameNativeIdentity(markerIdentity,nativeIdentity(marker.value,false))&&
            sameNativeIdentity(rootIdentity,nativeIdentity(temporaryRoot.value,true))&&currentSid()==sid,
            "Retained Hosted admission authority changed");
        NativeHandle reopened(CreateFileW(temporaryPath.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        require(reopened.value!=INVALID_HANDLE_VALUE&&sameNativeIdentity(rootIdentity,nativeIdentity(reopened.value,true)),"Hosted temporary root path was rebound");
    }
    void normalAppAllowed(const explorer::PrivateDesktop& desktop) const {
        verify();bool isolated=false,visible=true;
        succeeded(desktop.verifyIsolation(&isolated),"Verify private desktop before normal profile App construction");
        succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Read original input desktop before normal App construction");
        require(desktop.ready()&&explorer::PrivateDesktop::current()==&desktop&&isolated&&!visible,
            "Normal App creation requires the current isolated private desktop and fresh Hosted authority");
    }
};
}
