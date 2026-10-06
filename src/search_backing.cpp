#include "explorer/search_backing.hpp"
#include <shlobj.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <utility>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
HRESULT nativeError() noexcept { const auto e=GetLastError(); return HRESULT_FROM_WIN32(e?e:ERROR_GEN_FAILURE); }
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& other) noexcept {
        if(this!=&other){reset();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;
    }
    ~Handle(){reset();}
    void reset() noexcept {if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    explicit operator bool() const noexcept {return value!=INVALID_HANDLE_VALUE;}
};
bool sameId(const FILE_ID_INFO& a,const FILE_ID_INFO& b) noexcept {
    return a.VolumeSerialNumber==b.VolumeSerialNumber&&std::memcmp(a.FileId.Identifier,b.FileId.Identifier,16)==0;
}
bool sameBasic(const FILE_BASIC_INFO& a,const FILE_BASIC_INFO& b) noexcept {
    return a.CreationTime.QuadPart==b.CreationTime.QuadPart&&a.LastWriteTime.QuadPart==b.LastWriteTime.QuadPart&&
           a.ChangeTime.QuadPart==b.ChangeTime.QuadPart&&a.FileAttributes==b.FileAttributes;
}
HRESULT metadata(HANDLE handle,FILE_ID_INFO& id,FILE_BASIC_INFO& basic) noexcept {
    if(!GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id)))return nativeError();
    if(!GetFileInformationByHandleEx(handle,FileBasicInfo,&basic,sizeof(basic)))return nativeError();
    return (basic.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)?HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID):S_OK;
}
HRESULT verifyWrittenBytes(HANDLE read,const std::string& expected) noexcept {
    std::array<char,8192> buffer{};size_t offset=0;
    while(offset<expected.size()) {
        const auto amount=static_cast<DWORD>((std::min)(expected.size()-offset,buffer.size()));
        DWORD count=0;if(!ReadFile(read,buffer.data(),amount,&count,nullptr))return nativeError();
        if(count!=amount||std::memcmp(buffer.data(),expected.data()+offset,amount)!=0)return E_ACCESSDENIED;
        offset+=amount;
    }
    DWORD extra=0;if(!ReadFile(read,buffer.data(),1,&extra,nullptr))return nativeError();
    return extra?E_ACCESSDENIED:S_OK;
}
Handle openMetadata(const std::filesystem::path& path,DWORD access,bool directory=false) {
    return Handle(CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr));
}
struct RuleKey {
    std::vector<BYTE> pidl;
    bool recursive=false,excluded=false;
    bool operator==(const RuleKey&) const=default;
};
HRESULT captureKey(const std::vector<SearchScopeRule>& rules,std::vector<RuleKey>& output) {
    if(rules.empty()||rules.size()>256)return unsupported;
    std::vector<RuleKey> next;next.reserve(rules.size());
    for(const auto& rule:rules) {
        if(!rule.folder)return E_INVALIDARG;
        PIDLIST_ABSOLUTE raw=nullptr;const auto hr=SHGetIDListFromObject(rule.folder.Get(),&raw);
        struct Free {PIDLIST_ABSOLUTE p;~Free(){CoTaskMemFree(p);}} owned{raw};
        if(FAILED(hr))return hr;if(!raw)return E_UNEXPECTED;
        const auto size=ILGetSize(raw);if(size<sizeof(USHORT)||size>65536)return unsupported;
        const auto bytes=reinterpret_cast<const BYTE*>(raw);
        next.push_back({std::vector<BYTE>(bytes,bytes+size),rule.recursive,rule.excluded});
    }
    output=std::move(next);return S_OK;
}
HRESULT parseOwned(const std::filesystem::path& path,ComPtr<IShellItem>& item) {
    const auto hr=SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item));
    return FAILED(hr)?hr:item?S_OK:E_UNEXPECTED;
}
} // namespace

struct SearchBackingLease::Impl {
    std::filesystem::path path;
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    Handle read;
    HRESULT currentPath() const {
        auto pathHandle=openMetadata(path,FILE_READ_ATTRIBUTES);if(!pathHandle)return nativeError();
        FILE_ID_INFO currentId{};FILE_BASIC_INFO currentBasic{};
        const auto hr=metadata(pathHandle.value,currentId,currentBasic);
        return FAILED(hr)?hr:sameId(id,currentId)&&sameBasic(basic,currentBasic)?S_OK:E_ACCESSDENIED;
    }
};
SearchBackingLease::SearchBackingLease(std::unique_ptr<Impl> value) noexcept:impl_(std::move(value)){}
SearchBackingLease::~SearchBackingLease()=default;
const std::filesystem::path& SearchBackingLease::path() const noexcept{return impl_->path;}
const FILE_ID_INFO& SearchBackingLease::identity() const noexcept{return impl_->id;}

struct SearchBackingStore::Impl {
    DWORD creator=GetCurrentThreadId();
    std::filesystem::path parent,root;
    Handle directory;
    FILE_ID_INFO directoryId{};
    HRESULT initializationFailure=E_PENDING;
    bool closed=false,busy=false;
    void (*createdFileObserver)(const std::filesystem::path&,const FILE_ID_INFO&,void*)=nullptr;
    void* createdFileObserverContext=nullptr;
    struct Record {std::wstring query;std::vector<RuleKey> rules;std::shared_ptr<SearchBackingLease> lease;};
    std::vector<Record> records;
    explicit Impl(std::filesystem::path p):parent(std::move(p)){records.reserve(maximumBackings);}
    HRESULT currentDirectory() {
        auto pathHandle=openMetadata(root,FILE_READ_ATTRIBUTES,true);if(!pathHandle)return nativeError();
        FILE_ID_INFO id{};FILE_BASIC_INFO basic{};const auto hr=metadata(pathHandle.value,id,basic);
        if(FAILED(hr))return hr;
        return sameId(id,directoryId)&&(basic.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)?S_OK:E_ACCESSDENIED;
    }
    HRESULT initialize() {
        if(directory)return currentDirectory();
        // An owned-but-unproved created root is retained, not overwritten by
        // a successor GUID on each edit. A failed native setup stays failed.
        if(!root.empty())return FAILED(initializationFailure)?initializationFailure:E_ACCESSDENIED;
        if(parent.empty()) {
            std::array<wchar_t,32768> temp{};const auto size=GetTempPathW(static_cast<DWORD>(temp.size()),temp.data());
            if(!size||size>=temp.size())return nativeError();parent=temp.data();
        }
        if(!parent.is_absolute()||parent.native().find(L'\0')!=std::wstring::npos)return E_INVALIDARG;
        auto original=openMetadata(parent,FILE_READ_ATTRIBUTES,true);if(!original)return nativeError();
        FILE_ID_INFO parentId{};FILE_BASIC_INFO parentBasic{};auto hr=metadata(original.value,parentId,parentBasic);
        if(FAILED(hr))return hr;if(!(parentBasic.FileAttributes&FILE_ATTRIBUTE_DIRECTORY))return E_INVALIDARG;
        GUID guid{};if(FAILED(hr=CoCreateGuid(&guid)))return hr;
        wchar_t text[40]{};if(!StringFromGUID2(guid,text,40))return E_UNEXPECTED;
        auto next=parent/(std::wstring(L"WindowsExplorer-live-search-")+text);
        if(!CreateDirectoryW(next.c_str(),nullptr))return nativeError();
        // From here the exact created root is retained even on failure. No
        // recursive path delete and no cleanup of a colliding existing name.
        root=std::move(next);directory=openMetadata(root,FILE_READ_ATTRIBUTES|DELETE,true);
        if(!directory){initializationFailure=nativeError();return initializationFailure;}
        FILE_BASIC_INFO basic{};hr=metadata(directory.value,directoryId,basic);
        initializationFailure=FAILED(hr)?hr:(basic.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)?S_OK:E_ACCESSDENIED;
        return initializationFailure;
    }
};
SearchBackingStore::SearchBackingStore(std::filesystem::path parent):impl_(std::make_unique<Impl>(std::move(parent))){}
SearchBackingStore::~SearchBackingStore()=default;
size_t SearchBackingStore::retainedCount() const noexcept{return impl_->records.size();}
std::filesystem::path SearchBackingStore::directory() const{return impl_->root;}
void SearchBackingStore::setCreatedFileObserverForNativeTest(
    void (*observer)(const std::filesystem::path&,const FILE_ID_INFO&,void*),void* context) noexcept {
    impl_->createdFileObserver=observer;impl_->createdFileObserverContext=context;
}

HRESULT SearchBackingStore::build(const std::wstring& query,const std::vector<SearchScopeRule>& rules,SearchFolderBuild* result) {
    if(!result)return E_POINTER;
    auto& state=*impl_;
    if(GetCurrentThreadId()!=state.creator)return RPC_E_WRONG_THREAD;
    if(state.closed)return RO_E_CLOSED;
    if(state.busy)return HRESULT_FROM_WIN32(ERROR_BUSY);
    state.busy=true;struct Busy {bool& value;~Busy(){value=false;}} active{state.busy};
    try {
        bool required=false;std::vector<SearchScopeRule> descriptorRules;
        auto hr=searchScopeRulesRequireBacking(rules,&required,&descriptorRules);
        if(FAILED(hr))return hr;if(!required)return E_INVALIDARG;
        if(FAILED(hr=validateSearchDescriptorQuery(query)))return hr;
        std::vector<RuleKey> key;if(FAILED(hr=captureKey(descriptorRules,key)))return hr;
        for(const auto& record:state.records) if(record.lease&&record.lease->impl_->read&&record.query==query&&record.rules==key) {
            if(FAILED(hr=state.currentDirectory()))return hr;
            FILE_ID_INFO id{};FILE_BASIC_INFO basic{};hr=metadata(record.lease->impl_->read.value,id,basic);
            if(FAILED(hr))return hr;if(!sameId(id,record.lease->impl_->id)||!sameBasic(basic,record.lease->impl_->basic))return E_ACCESSDENIED;
            if(FAILED(hr=record.lease->impl_->currentPath()))return hr;
            SearchFolderBuild next;next.backing=record.lease;
            if(FAILED(hr=parseOwned(next.backing->path(),next.item)))return hr;
            if(state.closed||FAILED(hr=state.currentDirectory()))return state.closed?E_ABORT:hr;
            if(FAILED(hr=record.lease->impl_->currentPath()))return hr;
            *result=std::move(next);return S_OK;
        }
        if(state.records.size()>=maximumBackings)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
        if(FAILED(hr=state.initialize()))return hr;
        auto owned=std::make_unique<SearchBackingLease::Impl>();
        owned->path=state.root/(L"query-"+std::to_wstring(state.records.size())+L".search-ms");
        auto lease=std::shared_ptr<SearchBackingLease>(new SearchBackingLease(std::move(owned)));
        // Reserve before any writer COM callback. Failed/obsolete published
        // files are conservatively retained too; nothing deletes under a view.
        state.records.push_back({query,std::move(key),lease});
        SearchCreatedFileProof proof;
        hr=saveSearchForScopeRules(query,descriptorRules,lease->path(),SearchSaveMode::CreateNew,nullptr,nullptr,&proof);
        if(FAILED(hr))return hr;
        if(!proof.captured)return E_UNEXPECTED;
        // Retain only the writer-created identity. The path may have been
        // replaced while its exclusive handle was closed for native notify.
        auto& backing=*lease->impl_;backing.id=proof.identity;
        if(state.createdFileObserver)state.createdFileObserver(backing.path,proof.identity,state.createdFileObserverContext);
        if(state.closed||FAILED(hr=state.currentDirectory()))return state.closed?E_ABORT:hr;
        auto read=Handle(CreateFileW(backing.path.c_str(),GENERIC_READ|FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!read)return nativeError();
        FILE_ID_INFO readId{};FILE_BASIC_INFO readBasic{};
        if(FAILED(hr=metadata(read.value,readId,readBasic)))return hr;
        if(!sameId(proof.identity,readId)||(readBasic.FileAttributes&FILE_ATTRIBUTE_DIRECTORY))return E_ACCESSDENIED;
        // Denied-write sharing now pins the exact original bytes as well as
        // the object identity; a same-FileID rewrite during notify is rejected.
        if(FAILED(hr=verifyWrittenBytes(read.value,proof.bytes)))return hr;
        backing.basic=readBasic;backing.read=std::move(read);
        if(FAILED(hr=backing.currentPath()))return hr;
        SearchFolderBuild next;next.backing=lease;
        if(FAILED(hr=parseOwned(backing.path,next.item)))return hr;
        if(state.closed||FAILED(hr=state.currentDirectory()))return state.closed?E_ABORT:hr;
        if(FAILED(hr=backing.currentPath()))return hr;
        *result=std::move(next);return S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(const std::filesystem::filesystem_error&){return E_INVALIDARG;}
}

HRESULT SearchBackingStore::closeAfterNativeTeardown() noexcept {
    auto& state=*impl_;
    if(GetCurrentThreadId()!=state.creator)return RPC_E_WRONG_THREAD;
    if(state.busy)return HRESULT_FROM_WIN32(ERROR_BUSY);
    for(const auto& record:state.records)if(record.lease.use_count()!=1)return HRESULT_FROM_WIN32(ERROR_BUSY);
    state.closed=true;
    try {
        if(!state.directory)return state.root.empty()?S_OK:E_ACCESSDENIED;
        auto hr=state.currentDirectory();if(FAILED(hr))return hr;
        for(auto& record:state.records) {
            auto& lease=*record.lease->impl_;
            if(!lease.read) {
                // Failed pre-publication records may have no file. Even a
                // writer identity is insufficient without a matching retained
                // read lease; never adopt the current replacement as owned.
                const auto attrs=GetFileAttributesW(lease.path.c_str());
                if(attrs==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND)continue;
                return E_ACCESSDENIED;
            }
            auto removal=openMetadata(lease.path,FILE_READ_ATTRIBUTES|DELETE);if(!removal)return nativeError();
            FILE_ID_INFO id{};FILE_BASIC_INFO basic{};hr=metadata(removal.value,id,basic);
            if(FAILED(hr))return hr;if(!sameId(id,lease.id)||!sameBasic(basic,lease.basic))return E_ACCESSDENIED;
            FILE_DISPOSITION_INFO disposition{TRUE};
            if(!SetFileInformationByHandle(removal.value,FileDispositionInfo,&disposition,sizeof(disposition)))return nativeError();
            lease.read.reset();removal.reset();
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if(!SetFileInformationByHandle(state.directory.value,FileDispositionInfo,&disposition,sizeof(disposition)))return nativeError();
        state.directory.reset();state.records.clear();return S_OK;
    }catch(...){return E_FAIL;}
}

HRESULT buildSearchFolder(const std::wstring& query,const std::vector<SearchScopeRule>& rules,
                          SearchBackingStore* store,SearchFolderBuild* result) {
    if(!result)return E_POINTER;
    bool required=false;auto hr=searchScopeRulesRequireBacking(rules,&required);
    if(FAILED(hr))return hr;
    if(required)return store?store->build(query,rules,result):E_POINTER;
    SearchFolderBuild next;hr=createSearchFolderForScopeRules(query,rules,&next.item);
    if(SUCCEEDED(hr))*result=std::move(next);return hr;
}
} // namespace explorer
