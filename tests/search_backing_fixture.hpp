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

namespace explorer {
struct SearchBackingOwnershipNativeFixture {
    static void observe(SearchBackingStore& store,
        void (*callback)(const std::filesystem::path&,const FILE_ID_INFO&,void*),void* context) {
        store.setCreatedFileObserverForNativeTest(callback,context);
    }
};
} // namespace explorer

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

// Explicit test-owned cleanup only. Production never enumerates or deletes a
// published descriptor. This list uses the exact CREATE_NEW writer proof (or
// accepted original lease), and is invoked only after all test-native aliases
// and retained PIDLs are released and the actual creator worker drain passes.
// This is an explicit destructive disposable-fixture operation, never a proof
// that no independently retained Windows-native consumer could still exist.
struct DescriptorFiles {
    struct File {fs::path path;Id id;std::string original;FILE_BASIC_INFO originalBasic{};};
    struct Directory {fs::path path;Id id;};
    std::vector<File> files;
    std::vector<Directory> directories;
    static Id fromNative(const FILE_ID_INFO& value) {
        Id result{value.VolumeSerialNumber};std::copy_n(value.FileId.Identifier,16,result.file.begin());return result;
    }
    void note(const fs::path& path,const FILE_ID_INFO& proof) {
        const auto original=fromNative(proof);
        require(identity(path)==original,"test descriptor did not match exact writer/lease identity");
        for(const auto& value:files)if(value.path==path){require(value.id==original,"descriptor pathname was reused");return;}
        const auto parent=path.parent_path();const auto parentId=identity(parent);
        bool known=false;for(const auto& value:directories)if(value.path==parent){require(value.id==parentId,"descriptor directory changed");known=true;}
        if(!known)directories.push_back({parent,parentId});
        files.push_back({path,original,bytes(path),basic(path)});
    }
    void observe(explorer::SearchBackingStore& store) {
        explorer::SearchBackingOwnershipNativeFixture::observe(store,
            [](const fs::path& path,const FILE_ID_INFO& proof,void* context){
                static_cast<DescriptorFiles*>(context)->note(path,proof);
            },this);
    }
    void unchanged() const {
        for(const auto& value:files)require(identity(value.path)==value.id&&bytes(value.path)==value.original&&
            sameBasic(basic(value.path),value.originalBasic),"persistent descriptor FileID/bytes/basic metadata changed");
        for(const auto& value:directories)require(identity(value.path)==value.id,"persistent session directory identity changed");
    }
    void removeExplicitFixtureFiles() {
        exact(explorer::drainStaWorkers(5000),"explicit fixture final native drain before owned cleanup");
        unchanged();
        for(const auto& value:files) {
            const auto handle=CreateFileW(value.path.c_str(),FILE_READ_ATTRIBUTES|DELETE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            require(handle!=INVALID_HANDLE_VALUE,"open exact owned fixture descriptor for explicit cleanup");
            FILE_ID_INFO id{};FILE_BASIC_INFO info{};
            const auto identified=GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id));
            const auto described=identified?GetFileInformationByHandleEx(handle,FileBasicInfo,&info,sizeof(info)):FALSE;
            if(!identified||!described||fromNative(id)!=value.id||
               (info.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!sameBasic(info,value.originalBasic)) {
                CloseHandle(handle);require(false,"explicit descriptor cleanup refused an unproved object");
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            const auto removed=SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof(disposition));
            const auto error=removed?ERROR_SUCCESS:GetLastError();CloseHandle(handle);
            exact(HRESULT_FROM_WIN32(error),"explicit remove of exact test-owned original descriptor");
        }
        for(const auto& value:directories) {
            const auto handle=CreateFileW(value.path.c_str(),FILE_READ_ATTRIBUTES|DELETE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            require(handle!=INVALID_HANDLE_VALUE,"open exact owned fixture directory for explicit cleanup");
            FILE_ID_INFO id{};FILE_BASIC_INFO info{};
            const auto identified=GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id));
            const auto described=identified?GetFileInformationByHandleEx(handle,FileBasicInfo,&info,sizeof(info)):FALSE;
            if(!identified||!described||fromNative(id)!=value.id||
               !(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
                CloseHandle(handle);require(false,"explicit directory cleanup refused an unproved object");
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            const auto removed=SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof(disposition));
            const auto error=removed?ERROR_SUCCESS:GetLastError();CloseHandle(handle);
            exact(HRESULT_FROM_WIN32(error),"remove only now-empty exact test-owned session directory");
        }
        files.clear();directories.clear();
    }
};

struct NativePidlDeleter {
    using pointer=LPITEMIDLIST;
    void operator()(pointer value) const noexcept {CoTaskMemFree(value);}
};
using NativePidl=std::unique_ptr<ITEMIDLIST,NativePidlDeleter>;

// Deliberately has no plain SearchBackingLease. Genuine native references and
// a serialized original PIDL must retain a usable original backing path even
// after the Store LRU evicts it and the originating Store/App has closed.
struct DelayedNativeReader {
    ComPtr<IShellItem> item;
    ComPtr<IShellFolder> folder;
    ComPtr<IEnumIDList> enumeration;
    NativePidl pidl;
    bool onceOriginalCursorConsumed=false;
    void capture(IShellItem* original) {
        require(!item&&!folder&&!enumeration&&!pidl&&!onceOriginalCursorConsumed,"delayed original cursor capture must be fresh and unused");
        require(original!=nullptr,"delayed reader original native item missing");item=original;
        PIDLIST_ABSOLUTE raw=nullptr;const auto identified=SHGetIDListFromObject(original,&raw);pidl.reset(raw);
        exact(identified,"retain original actual descriptor PIDL");require(pidl!=nullptr,"delayed original native PIDL missing");
        exact(original->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)),"retain independent original native search folder");
        require(folder!=nullptr,"delayed native folder missing");
        exact(folder->EnumObjects(nullptr,static_cast<SHCONTF>(SHCONTF_FOLDERS|SHCONTF_NONFOLDERS),&enumeration),
              "retain actual original native result enumerator");
        require(enumeration!=nullptr,"delayed native enumerator missing");
    }
    void verify(const std::set<Id>& expected) {
        // This exact original enumerator has never been advanced. Its first
        // Next after owner close is the native lifetime proof; Reset is not
        // required by this provider and no fresh enumeration substitutes for it.
        require(enumeration!=nullptr&&!onceOriginalCursorConsumed,"original delayed native cursor already consumed");
        onceOriginalCursorConsumed=true;
        HRESULT originalFirstNext=E_PENDING;unsigned originalNextCalls=0;
        std::set<Id> actual;unsigned count=0;
        for(;;) {
            PITEMID_CHILD raw=nullptr;const auto next=enumeration->Next(1,&raw,nullptr);NativePidl owned(raw);
            if(!originalNextCalls++){originalFirstNext=next;
                std::cout<<"DelayedNativeReader first-use originalResetCalls=0 originalNextCalls=1"
                    <<" originalFirstNextHRESULT="<<static_cast<ULONG>(next)
                    <<" onceOriginalCursorConsumed="<<onceOriginalCursorConsumed<<'\n';}
            if(next==S_FALSE)break;
            exact(next,"read independently retained original native enumerator");require(owned!=nullptr&&++count<=8,"delayed row missing/excessive");
            ComPtr<IShellItem> row;exact(SHCreateItemWithParent(nullptr,folder.Get(),owned.get(),IID_PPV_ARGS(&row)),"delayed actual native row");
            require(row!=nullptr,"delayed native row missing");
            PWSTR path=nullptr;const auto named=row->GetDisplayName(SIGDN_FILESYSPATH,&path);
            struct Text {PWSTR p;~Text(){CoTaskMemFree(p);}} text{path};exact(named,"delayed actual physical result path");
            require(path&&actual.insert(identity(fs::path(path))).second,"delayed result missing/duplicate full FileID");
        }
        require(actual==expected,"original enumerator changed full owned result FileIDs after close");
        std::cout<<"DelayedNativeReader originalCursorCapturedUnused=1 originalResetCalls=0"
            <<" originalFirstNextHRESULT="<<static_cast<ULONG>(originalFirstNext)
            <<" originalNextCalls="<<originalNextCalls<<" exactFullFileIDs="<<actual.size()
            <<" onceOriginalCursorConsumed="<<onceOriginalCursorConsumed<<'\n';
        // The retained-item and PIDL binding checks run only AFTER consuming
        // the original cursor; they cannot supply/replace its first Next.
        awaitResults(item.Get(),expected);
        ComPtr<IShellItem> rebound;exact(SHCreateItemFromIDList(pidl.get(),IID_PPV_ARGS(&rebound)),"rebind original serialized native PIDL after close");
        require(rebound!=nullptr,"delayed original PIDL no longer binds");awaitResults(rebound.Get(),expected);
    }
    void release() {enumeration.Reset();folder.Reset();item.Reset();pidl.reset();}
};

} // namespace backing_test
