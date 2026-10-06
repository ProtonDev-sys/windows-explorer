#include "search_backing_fixture.hpp"
#include <cstdlib>

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
        // Park in this fresh GUID-owned fixture root. No store deletion
        // authority or target-directory sharing error substitutes for the
        // actual replacement/same-object-rewrite control.
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
        require(store.build(query,scopeRules,&output)==E_ACCESSDENIED&&output.item.Get()==preserved&&!output.backing&&store.retainedCount()==0,
                "writer-path replacement gained publication/ownership or changed output");
        require(boundary.entered&&identity(boundary.path)==boundary.replacement&&
                bytes(boundary.path)=="owned replacement must survive refused publication/cleanup"&&
                (!replace||(identity(boundary.parked)==boundary.original&&bytes(boundary.parked)==boundary.originalBytes)),
                "actual writer/replacement truth was lost after refused build");
        output={};exact(explorer::drainStaWorkers(5000),"notification-boundary original native drain");
        require(store.closeAfterNativeTeardown()==S_OK&&identity(boundary.path)==boundary.replacement&&
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
    Fixture fixture;DescriptorFiles descriptors;explorer::SearchBackingStore store(fixture.root);
    descriptors.observe(store);
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
    // Keep the Store accepting searches here; final close below proves
    // persistent paths after Store ownership and all ordinary aliases end.
    require(fs::exists(firstPath),"active native descriptor disappeared");
    const auto writer=CreateFileW(firstPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    const auto writeError=writer==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    require(writeError==ERROR_SHARING_VIOLATION,"live backing lease admitted competing bytes write");
    SearchFolderBuild normal;exact(explorer::buildSearchFolder(query,{{item(fixture.scope),false,false}},nullptr,&normal),"unchanged physical factory route");
    require(normal.item&&!normal.backing&&store.retainedCount()==3,"normal factory created backing");awaitResults(normal.item.Get(),{fixture.directId});
    require(bytes(firstPath)==originalBytes&&identity(firstPath)==firstId,"history/refinement replaced old backing");fixture.unchanged();
    normal={};first={};repeated={};second={};refined={};imported={};shallow.clear();deep.clear();
    exact(explorer::drainStaWorkers(5000),"native creator worker drain");
    const auto directory=store.directory();exact(store.closeAfterNativeTeardown(),"close only after real native aliases/drain");
    require(fs::exists(directory)&&identity(firstPath)==firstId&&bytes(firstPath)==originalBytes&&store.retainedCount()==0,
            "Store close removed or changed published native descriptor");
    descriptors.removeExplicitFixtureFiles();fixture.remove=true;
}
void residentAndFailurePreservation(){
    Fixture fixture;DescriptorFiles descriptors;explorer::SearchBackingStore store(fixture.root);
    descriptors.observe(store);auto scopeRules=rules(fixture,false,false);
    SearchFolderBuild output;const auto invalid=std::wstring(L"bad\0query",9);
    require(explorer::buildSearchFolder(invalid,scopeRules,&store,&output)==E_INVALIDARG&&store.retainedCount()==0&&store.directory().empty(),
            "invalid query created backing files");
    auto excluded=scopeRules;excluded[0].excluded=true;
    require(explorer::buildSearchFolder(L"name",excluded,&store,&output)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)&&store.retainedCount()==0,
            "unsupported virtual exclusion was silently dropped");
    DelayedNativeReader external;fs::path firstPath;Id firstId;std::string firstBytes;
    const auto queryFor=[&](size_t index){return L"System.FileName:=\""+fixture.directFile.filename().native()+
        L"\" AND System.Size:<="+std::to_wstring(1048576+index);};
    for(size_t index=0;index<explorer::SearchBackingStore::maximumResidentBackings+2;++index){
        SearchFolderBuild build;exact(explorer::buildSearchFolder(queryFor(index),scopeRules,&store,&build),"create distinct native descriptor beyond old lifetime cap");
        require(build.item&&build.backing,"actual distinct descriptor not paired with lease/item");
        awaitResults(build.item.Get(),{fixture.directId});
        require(store.retainedCount()==std::min(index+1,explorer::SearchBackingStore::maximumResidentBackings),"resident cache exceeded exact128 bound");
        if(index==0){firstPath=build.backing->path();firstId=identity(firstPath);firstBytes=bytes(firstPath);external.capture(build.item.Get());}
        if(index==129)output=std::move(build);
    }
    require(descriptors.files.size()==130&&store.retainedCount()==128,"130 lifetime intents did not complete with bounded128 cache");
    require(identity(firstPath)==firstId&&bytes(firstPath)==firstBytes,"LRU eviction changed first original backing");
    const auto latest=output.backing.get();const auto latestPath=output.backing->path();
    const auto latestId=identity(latestPath);const auto latestBytes=bytes(latestPath);
    require(explorer::buildSearchFolder(invalid,scopeRules,&store,&output)==E_INVALIDARG&&output.backing.get()==latest&&
        identity(latestPath)==latestId&&bytes(latestPath)==latestBytes,"real validation failure changed active output");
    SearchFolderBuild reused;exact(explorer::buildSearchFolder(queryFor(129),scopeRules,&store,&reused),"reuse actual MRU at capacity");
    require(reused.backing.get()==latest&&store.retainedCount()==128&&descriptors.files.size()==130,"exact current key failed native cache reuse");
    SearchFolderBuild miss;exact(explorer::buildSearchFolder(queryFor(0),scopeRules,&store,&miss),"reconstruct evicted query under a fresh immutable identity");
    require(miss.backing->path()!=firstPath&&identity(miss.backing->path())!=firstId&&descriptors.files.size()==131&&store.retainedCount()==128,
            "evicted-key reconstruction reused or replaced original published identity");
    awaitResults(miss.item.Get(),{fixture.directId});
    // Hit the oldest remaining resident entry, then insert a new query. The
    // promoted original must survive that eviction; the following oldest key
    // must genuinely miss and receive a different identity.
    SearchFolderBuild promoted;exact(explorer::buildSearchFolder(queryFor(3),scopeRules,&store,&promoted),"promote actual oldest resident native query");
    require(promoted.backing->path()==descriptors.files[3].path&&identity(promoted.backing->path())==descriptors.files[3].id,
            "older resident hit reconstructed instead of reusing its original FileID");
    SearchFolderBuild newer;exact(explorer::buildSearchFolder(queryFor(130),scopeRules,&store,&newer),"insert after actual older-key MRU promotion");
    SearchFolderBuild promotedAgain;exact(explorer::buildSearchFolder(queryFor(3),scopeRules,&store,&promotedAgain),"verify older-key promotion survives next native eviction");
    require(promotedAgain.backing.get()==promoted.backing.get()&&descriptors.files.size()==132&&store.retainedCount()==128,
            "older resident promotion did not preserve original native lease identity");
    SearchFolderBuild nextEvicted;exact(explorer::buildSearchFolder(queryFor(4),scopeRules,&store,&nextEvicted),"reconstruct actual next-oldest evicted query");
    require(nextEvicted.backing->path()!=descriptors.files[4].path&&identity(nextEvicted.backing->path())!=descriptors.files[4].id&&
            descriptors.files.size()==133&&store.retainedCount()==128,"actual MRU order evicted the wrong original resident query");
    awaitResults(newer.item.Get(),{fixture.directId});awaitResults(nextEvicted.item.Get(),{fixture.directId});
    fixture.unchanged();descriptors.unchanged();
    output={};reused={};miss={};promoted={};newer={};promotedAgain={};nextEvicted={};scopeRules.clear();excluded.clear();
    exact(explorer::drainStaWorkers(5000),"actual creator drain before Store close");
    const auto directory=store.directory();exact(store.closeAfterNativeTeardown(),"retire only bounded Store cache ownership");
    require(store.retainedCount()==0&&fs::exists(directory)&&identity(firstPath)==firstId&&bytes(firstPath)==firstBytes,
            "Store close removed or rewrote independently retained original descriptor");
    // All independent native references, including the enumerator and PIDL,
    // remain usable while the plain originating Store has already closed.
    external.verify({fixture.directId});descriptors.unchanged();external.release();
    descriptors.removeExplicitFixtureFiles();fixture.remove=true;
}

} // namespace

int wmain(int argc,wchar_t**){
    if(argc!=1)return 2;
    explorer::PrivateDesktop desktop;const auto isolated=desktop.initialize();if(isolated!=S_OK)return 3;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 4;
    int failures=0;
    for(const auto& test:std::array<std::pair<const char*,void(*)()>,4>{{
        {"actual native backed search routing, reuse, depth, refinement and lifetime",routingAndLifetime},
        {"130 distinct native descriptors, 128-resident eviction and delayed native binding after close",residentAndFailurePreservation},
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
