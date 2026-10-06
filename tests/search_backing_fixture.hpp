#pragma once
#include "explorer/search_backing.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <array>
#include <algorithm>
#include <compare>
#include <utility>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace backing_test {
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
using explorer::SearchScopeRule;
using explorer::SearchFolderBuild;
inline void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
inline void exact(HRESULT status,const char* label){
    if(status!=S_OK){std::cerr<<label<<" hr=0x"<<std::hex<<static_cast<unsigned long>(status)<<std::dec<<'\n';throw std::runtime_error(label);}
}
struct Id {
    ULONGLONG volume=0;std::array<BYTE,16> file{};
    auto operator<=>(const Id&) const=default;
};
inline Id identity(const fs::path& path){
    HANDLE handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"open owned native FileID");
    FILE_ID_INFO id{};const auto read=GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id));
    const auto error=read?ERROR_SUCCESS:GetLastError();CloseHandle(handle);exact(HRESULT_FROM_WIN32(error),"read owned native FileID");
    Id result{id.VolumeSerialNumber};std::copy_n(id.FileId.Identifier,16,result.file.begin());return result;
}
inline std::string bytes(const fs::path& path){
    std::ifstream stream(path,std::ios::binary);require(stream.good(),"read exclusively owned bytes");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
inline ComPtr<IShellItem> item(const fs::path& path){
    ComPtr<IShellItem> result;exact(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&result)),"create actual owned native item");
    require(result!=nullptr,"owned native item is missing");return result;
}
inline FILE_BASIC_INFO basic(const fs::path& path){
    const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"open original source metadata");FILE_BASIC_INFO result{};
    const auto read=GetFileInformationByHandleEx(handle,FileBasicInfo,&result,sizeof(result));const auto error=read?ERROR_SUCCESS:GetLastError();CloseHandle(handle);
    exact(HRESULT_FROM_WIN32(error),"read actual source metadata");return result;
}
inline bool sameBasic(const FILE_BASIC_INFO& a,const FILE_BASIC_INFO& b){
    return a.CreationTime.QuadPart==b.CreationTime.QuadPart&&a.LastWriteTime.QuadPart==b.LastWriteTime.QuadPart&&
        a.ChangeTime.QuadPart==b.ChangeTime.QuadPart&&a.FileAttributes==b.FileAttributes;
}
struct Fixture {
    fs::path root,scope,nested,directFile,nestedFile,extraFile;
    std::wstring prefix;
    Id directId,nestedId,extraId;
    FILE_BASIC_INFO directBasic{},nestedBasic{},extraBasic{};
    bool remove=false;
    explicit Fixture(bool three=false){
        GUID id{};exact(CoCreateGuid(&id),"create owned test GUID");wchar_t text[40]{};require(StringFromGUID2(id,text,40)!=0,"format GUID");
        prefix=std::wstring(L"backed-owned-")+text;
        root=fs::temp_directory_path()/(prefix+L"-資料");require(fs::create_directory(root),"create fresh root");
        scope=root/L"owned-検索";nested=scope/L"nested";require(fs::create_directory(scope)&&fs::create_directory(nested),"create only owned scopes");
        directFile=scope/(prefix+L"-direct.txt");nestedFile=nested/(prefix+L"-nested.txt");
        for(const auto& path:{directFile,nestedFile}){std::ofstream out(path,std::ios::binary);out<<"owned immutable search source";require(out.good(),"write owned source");}
        directId=identity(directFile);nestedId=identity(nestedFile);directBasic=basic(directFile);nestedBasic=basic(nestedFile);
        if(three){extraFile=scope/(prefix+L"-other.txt");std::ofstream out(extraFile,std::ios::binary);out<<"owned immutable search source";require(out.good(),"write third owned source");out.close();extraId=identity(extraFile);extraBasic=basic(extraFile);}
    }
    void unchanged() const {
        require(identity(directFile)==directId&&identity(nestedFile)==nestedId&&
                bytes(directFile)=="owned immutable search source"&&bytes(nestedFile)=="owned immutable search source"&&sameBasic(basic(directFile),directBasic)&&sameBasic(basic(nestedFile),nestedBasic),"native backing changed original sources");
        if(!extraFile.empty())require(identity(extraFile)==extraId&&bytes(extraFile)=="owned immutable search source"&&sameBasic(basic(extraFile),extraBasic),"third owned source changed");
    }
    ~Fixture(){
        if(!remove){std::wcerr<<L"Preserved failed owned fixture: "<<root.native()<<L'\n';return;}
        std::error_code ignored;
        // No recursive delete; a foreign/new child keeps the root nonempty.
        if(!extraFile.empty())fs::remove(extraFile,ignored);
        fs::remove(directFile,ignored);fs::remove(nestedFile,ignored);fs::remove(nested,ignored);fs::remove(scope,ignored);fs::remove(root,ignored);
    }
};
inline std::set<Id> results(IShellItem* source){
    ComPtr<IShellFolder> folder;exact(source->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)),"bind real descriptor folder");
    ComPtr<IEnumIDList> enumeration;const auto started=folder->EnumObjects(nullptr,static_cast<SHCONTF>(SHCONTF_FOLDERS|SHCONTF_NONFOLDERS),&enumeration);
    require(started==S_OK||started==S_FALSE,"native result enumeration failed");
    std::set<Id> ids;unsigned count=0;
    while(enumeration){
        PITEMID_CHILD raw=nullptr;const auto next=enumeration->Next(1,&raw,nullptr);
        struct Pidl{PITEMID_CHILD p;~Pidl(){CoTaskMemFree(p);}} pidl{raw};
        if(next==S_FALSE)break;exact(next,"native result identity");require(raw!=nullptr&&++count<=8,"missing or excessive result identity");
        ComPtr<IShellItem> child;exact(SHCreateItemWithParent(nullptr,folder.Get(),raw,IID_PPV_ARGS(&child)),"create actual result");
        PWSTR path=nullptr;const auto named=child->GetDisplayName(SIGDN_FILESYSPATH,&path);
        struct Text{PWSTR p;~Text(){CoTaskMemFree(p);}} owned{path};exact(named,"native result must be physical owned file");
        require(path&&ids.insert(identity(fs::path(path))).second,"missing or duplicate full FileID");
    }
    return ids;
}
inline void awaitResults(IShellItem* source,const std::set<Id>& expected){
    const auto deadline=GetTickCount64()+5000;
    for(;;){const auto actual=results(source);if(actual==expected)return;if(GetTickCount64()>=deadline){require(actual==expected,"complete native FileIDs/depth mismatch");}Sleep(10);}
}
inline std::vector<SearchScopeRule> rules(Fixture& fixture,bool physicalRecursive,bool virtualRecursive){
    ComPtr<IShellItem> control;exact(SHGetKnownFolderItem(FOLDERID_ControlPanelFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&control)),"exact public known folder");
    return {{control,virtualRecursive,false},{item(fixture.scope),physicalRecursive,false}};
}
} // namespace backing_test
