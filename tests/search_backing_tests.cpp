#include "search_backing_fixture.hpp"
#include <cstdlib>

namespace explorer {
struct SearchBackingOwnershipNativeFixture {
    static void observe(SearchBackingStore& store,
        void (*callback)(const std::filesystem::path&,const FILE_ID_INFO&,void*),void* context) {
        store.setCreatedFileObserverForNativeTest(callback,context);
    }
};
} // namespace explorer

namespace {
using namespace backing_test;
Id proofIdentity(const FILE_ID_INFO& native){
    Id result{native.VolumeSerialNumber};std::copy_n(native.FileId.Identifier,16,result.file.begin());return result;
}
void removeExactOwnedFile(const fs::path& path,const Id& expected){
    const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"open fixture-owned exact removal");
    FILE_ID_INFO native{};const auto captured=GetFileInformationByHandleEx(handle,FileIdInfo,&native,sizeof(native));
    const auto error=captured?ERROR_SUCCESS:GetLastError();
    if(error!=ERROR_SUCCESS||proofIdentity(native)!=expected){CloseHandle(handle);require(false,"fixture-owned removal identity mismatch");}
    FILE_DISPOSITION_INFO disposition{TRUE};const auto removed=SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof(disposition));
    const auto removalError=removed?ERROR_SUCCESS:GetLastError();CloseHandle(handle);
    exact(HRESULT_FROM_WIN32(removalError),"remove only fixture-owned native object");
}
void writerOwnershipAndNotificationBoundary(bool replace){
    Fixture fixture;auto scopeRules=rules(fixture,false,false);
    const auto query=L"System.FileName:=\""+fixture.directFile.filename().native()+L"\"";
    struct Boundary {
        explorer::SearchBackingStore* store=nullptr;
        const std::vector<SearchScopeRule>* rules=nullptr;
        std::wstring query;
        fs::path path,parked;
        Id original,replacement;
        std::string originalBytes;
        bool entered=false,replace=false;
    } boundary;
    {
        explorer::SearchBackingStore store(fixture.root);
        boundary.store=&store;boundary.rules=&scopeRules;boundary.query=query;boundary.replace=replace;
        // The store deliberately retains DELETE access on its directory.
        // Park outside that held directory, in this fresh GUID-owned root;
        // a target-directory sharing error must not substitute for replacement.
        boundary.parked=fixture.root/(fixture.prefix+L"-writer-original.search-ms");
        explorer::SearchBackingOwnershipNativeFixture::observe(store,
            [](const fs::path& path,const FILE_ID_INFO& proof,void* context){
                auto& state=*static_cast<Boundary*>(context);require(!state.entered,"recursive notification-boundary observer");
                state.entered=true;state.path=path;
                state.original=proofIdentity(proof);require(identity(path)==state.original,"proof did not capture actual writer object");
                state.originalBytes=bytes(path);
                SearchFolderBuild nested;
                require(state.store->build(state.query,*state.rules,&nested)==HRESULT_FROM_WIN32(ERROR_BUSY)&&!nested.item&&!nested.backing,
                        "native notification-boundary reentry published nested output");
                require(state.store->closeAfterNativeTeardown()==HRESULT_FROM_WIN32(ERROR_BUSY)&&fs::exists(path),
                        "native notification-boundary reentry removed writer file");
                if(state.replace) {
                    const auto moved=MoveFileExW(path.c_str(),state.parked.c_str(),0);
                    const auto moveError=moved?ERROR_SUCCESS:GetLastError();
                    if(!moved)std::cerr<<"owned writer rename GetLastError="<<moveError<<'\n';
                    exact(HRESULT_FROM_WIN32(moveError),"rename actual owned writer object at notification boundary");
                    require(identity(state.parked)==state.original,"renamed writer identity changed");
                }
                const auto replacement=CreateFileW(path.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES|DELETE,0,nullptr,
                    state.replace?CREATE_NEW:TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                require(replacement!=INVALID_HANDLE_VALUE,"create actual replacement or same-object rewrite");
                const std::string replacementBytes="owned replacement must survive refused publication/cleanup";
                DWORD written=0;const auto saved=WriteFile(replacement,replacementBytes.data(),static_cast<DWORD>(replacementBytes.size()),&written,nullptr);
                FILE_ID_INFO replacementId{};
                const auto captured=GetFileInformationByHandleEx(replacement,FileIdInfo,&replacementId,sizeof(replacementId));
                const auto flushed=FlushFileBuffers(replacement);CloseHandle(replacement);
                require(saved&&written==replacementBytes.size()&&captured&&flushed,"write/capture real replacement");
                state.replacement=proofIdentity(replacementId);
                require(state.replace?state.replacement!=state.original:state.replacement==state.original,
                        "replacement/rewrite full FileID is not the actual expected object");
            },&boundary);
        SearchFolderBuild output;output.item=item(fixture.scope);const auto preserved=output.item.Get();
        require(store.build(query,scopeRules,&output)==E_ACCESSDENIED&&output.item.Get()==preserved&&!output.backing&&store.retainedCount()==1,
                "writer-path replacement gained publication/ownership or changed output");
        require(boundary.entered&&identity(boundary.path)==boundary.replacement&&
                bytes(boundary.path)=="owned replacement must survive refused publication/cleanup"&&
                (!replace||(identity(boundary.parked)==boundary.original&&bytes(boundary.parked)==boundary.originalBytes)),
                "actual writer/replacement truth was lost after refused build");
        output={};exact(explorer::drainStaWorkers(5000),"notification-boundary original native drain");
        require(store.closeAfterNativeTeardown()==E_ACCESSDENIED&&identity(boundary.path)==boundary.replacement&&
                (!replace||identity(boundary.parked)==boundary.original),
                "unproved replacement gained native cleanup authority");
    }
    require(identity(boundary.path)==boundary.replacement&&(!replace||identity(boundary.parked)==boundary.original),
            "conservative failed-store destruction deleted original/replacement");
    fixture.unchanged();scopeRules.clear();
    removeExactOwnedFile(boundary.path,boundary.replacement);
    if(replace)removeExactOwnedFile(boundary.parked,boundary.original);
    require(fs::remove(boundary.path.parent_path()),"remove now-empty owned store directory");fixture.remove=true;
}
void writerOwnershipReplacement(){writerOwnershipAndNotificationBoundary(true);}
void writerOwnershipRewrite(){writerOwnershipAndNotificationBoundary(false);}
void routingAndLifetime(){
    Fixture fixture;explorer::SearchBackingStore store(fixture.root);
    const auto query=L"System.FileName:~<\""+fixture.prefix+L"\"";
    auto shallow=rules(fixture,false,true),deep=rules(fixture,true,false);
    bool required=false;exact(explorer::searchScopeRulesRequireBacking(shallow,&required),"classify mixed shallow");require(required,"mixed shallow lacks native descriptor route");
    SearchFolderBuild first;exact(explorer::buildSearchFolder(query,shallow,&store,&first),"build actual backed shallow folder");
    require(first.item&&first.backing&&store.retainedCount()==1,"native result/lease not paired");
    const auto firstPath=first.backing->path();const auto firstId=identity(firstPath);const auto originalBytes=bytes(firstPath);
    awaitResults(first.item.Get(),{fixture.directId});
    explorer::SavedSearchMetadata imported;exact(explorer::readSavedSearch(firstPath,&imported),"import exact ephemeral descriptor rules");
    require(imported.scopeRules.size()==2&&imported.scopeRules[0].recursive&&!imported.scopeRules[1].recursive,"ordered individual depths lost");
    for(size_t index=0;index<shallow.size();++index){int order=1;exact(imported.scopeRules[index].folder->Compare(shallow[index].folder.Get(),SICHINT_CANONICAL,&order),"canonical original scope");require(order==0,"original native scope changed");}
    SearchFolderBuild repeated;exact(explorer::buildSearchFolder(query,shallow,&store,&repeated),"repeat same actual query/rules");
    require(repeated.backing.get()==first.backing.get()&&identity(repeated.backing->path())==firstId&&store.retainedCount()==1,"exact repeated query allocated or replaced backing");
    SearchFolderBuild second;exact(explorer::buildSearchFolder(query,deep,&store,&second),"build different individual depths");
    require(store.retainedCount()==2&&second.backing->path()!=firstPath,"different depths reused wrong native identity");awaitResults(second.item.Get(),{fixture.directId,fixture.nestedId});
    SearchFolderBuild refined;const auto precise=L"System.FileName:=\""+fixture.directFile.filename().native()+L"\"";
    exact(explorer::buildSearchFolder(precise,deep,&store,&refined),"refine same exact native rules");awaitResults(refined.item.Get(),{fixture.directId});
    require(store.closeAfterNativeTeardown()==HRESULT_FROM_WIN32(ERROR_BUSY)&&fs::exists(firstPath),"active leases allowed backing deletion");
    const auto writer=CreateFileW(firstPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    const auto writeError=writer==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    require(writeError==ERROR_SHARING_VIOLATION,"live backing lease admitted competing bytes write");
    SearchFolderBuild normal;exact(explorer::buildSearchFolder(query,{{item(fixture.scope),false,false}},nullptr,&normal),"unchanged physical factory route");
    require(normal.item&&!normal.backing&&store.retainedCount()==3,"normal factory created backing");awaitResults(normal.item.Get(),{fixture.directId});
    require(bytes(firstPath)==originalBytes&&identity(firstPath)==firstId,"history/refinement replaced old backing");fixture.unchanged();
    normal={};first={};repeated={};second={};refined={};imported={};shallow.clear();deep.clear();
    exact(explorer::drainStaWorkers(5000),"native creator worker drain");
    const auto directory=store.directory();exact(store.closeAfterNativeTeardown(),"close only after real native aliases/drain");
    require(!fs::exists(directory),"owned native backing cleanup leaked files");fixture.remove=true;
}
void boundAndFailurePreservation(){
    Fixture fixture;explorer::SearchBackingStore store(fixture.root);auto scopeRules=rules(fixture,false,false);
    SearchFolderBuild output;const auto invalid=std::wstring(L"bad\0query",9);
    require(explorer::buildSearchFolder(invalid,scopeRules,&store,&output)==E_INVALIDARG&&store.retainedCount()==0&&store.directory().empty(),"invalid query created backing files");
    auto excluded=scopeRules;excluded[0].excluded=true;
    require(explorer::buildSearchFolder(L"name",excluded,&store,&output)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)&&store.retainedCount()==0,"unsupported virtual exclusion was silently dropped");
    fs::path firstPath;Id firstId;std::string firstBytes;
    for(size_t index=0;index<explorer::SearchBackingStore::maximumBackings;++index){
        SearchFolderBuild build;const auto query=L"System.FileName:=\""+fixture.prefix+L"-bound-"+std::to_wstring(index)+L".txt\"";
        exact(explorer::buildSearchFolder(query,scopeRules,&store,&build),"create bounded actual descriptors");
        if(index==0){firstPath=build.backing->path();firstId=identity(firstPath);firstBytes=bytes(firstPath);output=std::move(build);}
    }
    require(store.retainedCount()==explorer::SearchBackingStore::maximumBackings,"bounded native descriptor count mismatch");
    const auto original=output.backing.get();
    require(explorer::buildSearchFolder(L"System.FileName:=\"overflow-new-query\"",scopeRules,&store,&output)==HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES)&&
            output.backing.get()==original&&identity(firstPath)==firstId&&bytes(firstPath)==firstBytes,"reached bound changed active file/output");
    SearchFolderBuild reused;const auto firstQuery=L"System.FileName:=\""+fixture.prefix+L"-bound-0.txt\"";
    exact(explorer::buildSearchFolder(firstQuery,scopeRules,&store,&reused),"reuse exact existing descriptor at capacity");
    require(reused.backing.get()==original&&store.retainedCount()==explorer::SearchBackingStore::maximumBackings,"capacity reuse changed identity/count");
    fixture.unchanged();output={};reused={};scopeRules.clear();excluded.clear();
    exact(explorer::drainStaWorkers(5000),"bounded fixture native drain");const auto directory=store.directory();
    exact(store.closeAfterNativeTeardown(),"close bounded owned descriptors");require(!fs::exists(directory),"bounded cleanup incomplete");fixture.remove=true;
}
} // namespace

int wmain(int argc,wchar_t**){
    if(argc!=1)return 2;
    explorer::PrivateDesktop desktop;const auto isolated=desktop.initialize();if(isolated!=S_OK)return 3;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 4;
    int failures=0;
    for(const auto& test:std::array<std::pair<const char*,void(*)()>,4>{{
        {"actual native backed search routing, reuse, depth, refinement and lifetime",routingAndLifetime},
        {"128 actual descriptors, reached-bound and failure preservation",boundAndFailurePreservation},
        {"actual writer FileID, post-notification native replacement/reentry and refused cleanup",writerOwnershipReplacement},
        {"actual same-FileID post-notification rewrite and refused cleanup",writerOwnershipRewrite}}}){
        try{test.second();std::cout<<"PASS: "<<test.first<<'\n';}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL: "<<test.first<<": "<<error.what()<<'\n';}
    }
    const auto drained=explorer::drainStaWorkers(5000);
    if(FAILED(drained)){std::cerr<<"FAIL: actual final native worker drain\n";std::cerr.flush();TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);}
    bool inputUnchanged=false;bool visible=true;
    if(desktop.verifyIsolation(&inputUnchanged)!=S_OK||!inputUnchanged||desktop.visibleWindowsOnInputDesktop(visible)!=S_OK||visible)++failures;
    CoUninitialize();return failures?1:0;
}
