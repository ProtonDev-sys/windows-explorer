#include "explorer/native_apartment.hpp"
#include "explorer/app.hpp"
#include "search_backing_fixture.hpp"
#include <cstdlib>
#include <cwchar>
#include <windowsx.h>

namespace explorer {
struct SearchBackingNativeFixture {
    using Id=backing_test::Id;
    static inline ULONGLONG deadline=0;
    static void source(backing_test::Fixture& fixture) {
        backing_test::require(GetTickCount64()<deadline,"App fixture absolute 60000ms deadline");
        fixture.unchanged();
        const auto* desktop=PrivateDesktop::current();backing_test::require(desktop!=nullptr,"actual private creator desktop missing");
        bool input=false,visible=true;backing_test::exact(desktop->verifyIsolation(&input),"private/input desktop unchanged");
        backing_test::exact(desktop->visibleWindowsOnInputDesktop(visible),"no own visible input window");
        backing_test::require(input&&!visible,"App fixture entered the input desktop");
    }
    static void pump(ExplorerApp* app) {
        MSG message{};
        while(GetTickCount64()<deadline&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)continue;
            if(!app||!app->preprocess(message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
    }
    struct OwnedSource {
        Id id;
        std::filesystem::path originalPath;
        ComPtr<IShellItem> physical;
        std::wstring nativePath;
        Pidl physicalPidl;
    };
    struct RowReceipt {
        int row=0;
        HRESULT contextual=E_PENDING;
        int contextualOrder=1;
        bool physicalRawBytesEqual=false;
    };
    static std::vector<OwnedSource> originalSources(backing_test::Fixture& fixture) {
        std::vector<OwnedSource> owned;
        const std::array<std::pair<Id,std::filesystem::path>,3> files{{
            {fixture.directId,fixture.directFile},{fixture.nestedId,fixture.nestedFile},{fixture.extraId,fixture.extraFile}}};
        for(const auto& [id,path]:files) {
            source(fixture);
            OwnedSource value;value.id=id;value.originalPath=path;value.physical=backing_test::item(path);
            source(fixture);
            PWSTR rawPath=nullptr;const auto named=value.physical->GetDisplayName(SIGDN_FILESYSPATH,&rawPath);
            struct Text {PWSTR p;~Text(){CoTaskMemFree(p);}} nativePath{rawPath};
            backing_test::exact(named,"capture original physical native filesystem spelling");
            backing_test::require(nativePath.p&&*nativePath.p,"original physical filesystem path missing");
            value.nativePath=nativePath.p;
            source(fixture);
            PIDLIST_ABSOLUTE rawPidl=nullptr;const auto located=SHGetIDListFromObject(value.physical.Get(),&rawPidl);
            value.physicalPidl.reset(rawPidl);
            backing_test::exact(located,"capture original full physical filesystem PIDL");
            backing_test::require(value.physicalPidl!=nullptr,"original full physical filesystem PIDL missing");
            source(fixture);
            backing_test::require(backing_test::identity(path)==id,"original native source FileID changed during authority capture");
            owned.push_back(std::move(value));
        }
        source(fixture);return owned;
    }
    static bool complete(ExplorerApp& app,backing_test::Fixture& fixture,const std::wstring& query,
                         const std::vector<SearchScopeRule>& originalRules,const std::vector<OwnedSource>& owned,
                         const std::set<Id>& expected,ULONGLONG actionDeadline,bool diagnose=false,
                         std::vector<RowReceipt>* receipts=nullptr) {
        // Declare the lifetime guard before retained native interfaces so close
        // cannot retire their backing while a provider callback is on this stack.
        ExplorerApp::SearchNativeCallScope lifetime(app);
        const auto reject=[&](const char* reason,HRESULT hr=E_PENDING) {
            if(diagnose)std::cout<<"AppSearch failureDiagnostic reject="<<reason<<" HRESULT="<<static_cast<ULONG>(hr)<<'\n'<<std::flush;
            return false;
        };
        source(fixture);
        if(app.closing_||app.navigating_||!app.searchActive_||app.activeQuery_!=query||!app.view_||!app.folderView_||!app.currentPidl_)
            return reject("active-source");
        const auto view=app.view_;const auto folder=app.folderView_;const auto navigation=app.navigationCount_;
        Pidl current(ILCloneFull(app.currentPidl_.get()));backing_test::require(current!=nullptr,"retain original accepted native location");
        // Never retain a live App vector element across a provider call.
        const auto actualRules=app.searchScopeRules_;const auto expectedRules=originalRules;
        const auto currentSource=[&] {
            if(GetTickCount64()>=deadline||(!diagnose&&GetTickCount64()>=actionDeadline)||app.closing_||app.navigating_||!app.searchActive_||
               app.activeQuery_!=query||app.view_.Get()!=view.Get()||app.folderView_.Get()!=folder.Get()||app.navigationCount_!=navigation||
               !app.currentPidl_||!sameBytes(current.get(),app.currentPidl_.get())||app.searchScopeRules_.size()!=actualRules.size())return false;
            for(size_t index=0;index<actualRules.size();++index) {
                const auto& live=app.searchScopeRules_[index];const auto& saved=actualRules[index];
                if(live.folder.Get()!=saved.folder.Get()||live.recursive!=saved.recursive||live.excluded!=saved.excluded)return false;
            }
            return true;
        };
        const auto gate=[&] {
            if(!currentSource())return false;
            source(fixture);return currentSource();
        };
        if(actualRules.size()!=expectedRules.size())return reject("rule-count");
        for(size_t index=0;index<expectedRules.size();++index) {
            const auto a=actualRules[index];const auto b=expectedRules[index];
            if(!a.folder||!b.folder||!gate())return reject("rule-source");
            int order=1;const auto compared=a.folder->Compare(b.folder.Get(),SICHINT_CANONICAL,&order);
            if(diagnose)std::cout<<"AppSearch failureDiagnostic rule="<<index<<" canonicalHRESULT/order="<<static_cast<ULONG>(compared)<<"/"<<order<<'\n';
            if(!currentSource()||compared!=S_OK||order||a.recursive!=b.recursive||a.excluded!=b.excluded)return reject("canonical-original-rule",compared);
        }
        if(!gate())return reject("view-source");
        HWND window=nullptr;const auto windowRead=view->GetWindow(&window);
        if(!currentSource()||windowRead!=S_OK||!window||!IsChild(app.window_,window))return reject("native-view-window",windowRead);
        DWORD owner=0;const auto thread=GetWindowThreadProcessId(window,&owner);
        if(owner!=GetCurrentProcessId()||thread!=GetCurrentThreadId())return reject("native-view-window-owner-thread");
        if(!gate())return reject("count-source");
        int count=-1;const auto countRead=folder->ItemCount(SVGIO_ALLVIEW,&count);
        if(diagnose)std::cout<<"AppSearch failureDiagnostic countHRESULT/count/expected="<<static_cast<ULONG>(countRead)<<"/"<<count<<"/"<<expected.size()<<'\n';
        if(!currentSource()||countRead!=S_OK||count!=static_cast<int>(expected.size()))return reject("native-count",countRead);
        if(!gate())return reject("folder-source");
        ComPtr<IShellFolder> native;const auto folderRead=folder->GetFolder(IID_PPV_ARGS(&native));
        if(!currentSource()||folderRead!=S_OK||!native)return reject("native-folder",folderRead);
        if(!gate())return reject("desktop-folder-source");
        ComPtr<IShellFolder> desktopFolder;const auto desktopRead=SHGetDesktopFolder(&desktopFolder);
        if(!currentSource()||desktopRead!=S_OK||!desktopFolder)return reject("full-pidl-desktop-folder",desktopRead);
        std::set<Id> actual;std::vector<RowReceipt> verified;
        for(int index=0;index<count;++index) {
            if(!gate())return reject("row-source");
            PITEMID_CHILD raw=nullptr;const auto read=folder->Item(index,&raw);Pidl child(raw);
            if(!currentSource()||read!=S_OK||!child)return reject("native-child",read);
            if(!gate())return reject("row-create-source");
            ComPtr<IShellItem> row;const auto rowRead=SHCreateItemWithParent(nullptr,native.Get(),child.get(),IID_PPV_ARGS(&row));
            if(!currentSource()||rowRead!=S_OK||!row)return reject("native-row",rowRead);
            if(!gate())return reject("row-path-source");
            PWSTR rawPath=nullptr;const auto named=row->GetDisplayName(SIGDN_FILESYSPATH,&rawPath);
            struct Text {PWSTR p;~Text(){CoTaskMemFree(p);}} rowPath{rawPath};
            if(!currentSource()||named!=S_OK||!rowPath.p||!*rowPath.p)return reject("native-row-filesystem-path",named);
            // Native search children use their search-folder canonical context.
            // Their display path must first exactly match an ORIGINAL physical
            // item's own native spelling, including any normalized short alias.
            const OwnedSource* candidate=nullptr;
            for(const auto& original:owned)if(original.nativePath==rowPath.p) {
                if(candidate)return reject("ambiguous-original-native-path");candidate=&original;
            }
            if(!candidate||!expected.contains(candidate->id))return reject("native-row-path-not-expected-original");
            if(!gate())return reject("contextual-compare-source");
            int contextualOrder=1;const auto contextual=row->Compare(candidate->physical.Get(),SICHINT_CANONICAL,&contextualOrder);
            if(!currentSource())return reject("contextual-compare-stale");
            if(diagnose)std::cout<<"AppSearch failureDiagnostic row="<<index<<" contextualOnlyHRESULT/order="<<static_cast<ULONG>(contextual)<<"/"<<contextualOrder<<'\n';
            // This contextual comparison is evidence only. S_FALSE never grants
            // identity or permission to open the path. The physical authority
            // below independently requires exact canonical S_OK/order 0.
            if(!gate())return reject("physical-create-source");
            ComPtr<IShellItem> physical;
            const auto physicalRead=SHCreateItemFromParsingName(rowPath.p,nullptr,IID_PPV_ARGS(&physical));
            if(!currentSource()||physicalRead!=S_OK||!physical)return reject("physical-from-native-row-path",physicalRead);
            if(!gate())return reject("physical-compare-source");
            int physicalOrder=1;const auto compared=physical->Compare(candidate->physical.Get(),SICHINT_CANONICAL,&physicalOrder);
            if(!currentSource()||compared!=S_OK||physicalOrder)return reject("original-physical-canonical-identity",compared);
            if(!gate())return reject("physical-pidl-source");
            PIDLIST_ABSOLUTE rawPhysical=nullptr;const auto located=SHGetIDListFromObject(physical.Get(),&rawPhysical);Pidl physicalPidl(rawPhysical);
            const bool physicalRawBytesEqual=physicalPidl&&sameBytes(physicalPidl.get(),candidate->physicalPidl.get());
            const bool nativePhysicalEqual=physicalPidl&&ILIsEqual(physicalPidl.get(),candidate->physicalPidl.get());
            if(diagnose)std::cout<<"AppSearch failureDiagnostic row="<<index<<" physicalPIDL original/actualBytes="
                <<ILGetSize(candidate->physicalPidl.get())<<"/"<<(physicalPidl?ILGetSize(physicalPidl.get()):0)
                <<" rawBytesEqual="<<physicalRawBytesEqual<<" nativeILIsEqual="<<nativePhysicalEqual
                <<" physicalCanonicalHRESULT/order="<<static_cast<ULONG>(compared)<<"/"<<physicalOrder<<'\n'<<std::flush;
            if(!currentSource()||located!=S_OK||!physicalPidl||!nativePhysicalEqual)return reject("original-full-physical-native-il-equality",located);
            // Full absolute PIDLs are relative to the desktop Shell folder.
            // Public CompareIDs supplies the non-binary canonical authority;
            // raw opaque serialization is recorded without parsing its payload.
            if(!gate())return reject("full-physical-canonical-pidl-source");
            const auto fullCompared=desktopFolder->CompareIDs(SHCIDS_CANONICALONLY,physicalPidl.get(),candidate->physicalPidl.get());
            const auto fullOrder=static_cast<short>(HRESULT_CODE(fullCompared));
            if(diagnose)std::cout<<"AppSearch failureDiagnostic row="<<index<<" fullPhysicalCanonicalPIDLHRESULT/order="
                <<static_cast<ULONG>(fullCompared)<<"/"<<fullOrder<<'\n'<<std::flush;
            if(!currentSource()||fullCompared!=S_OK||fullOrder!=0)return reject("original-full-physical-canonical-pidl-identity",fullCompared);
            // Keep the real search PIDL and the actual IFolderView2 item as a
            // separate consistency proof; neither is relabeled a physical PIDL.
            if(!gate())return reject("view-row-item-source");
            ComPtr<IShellItem> viewRow;const auto viewRead=folder->GetItem(index,IID_PPV_ARGS(&viewRow));
            if(!currentSource()||viewRead!=S_OK||!viewRow)return reject("actual-view-row-item",viewRead);
            if(!gate())return reject("row-pidl-source");
            PIDLIST_ABSOLUTE rawRow=nullptr;const auto rowLocated=SHGetIDListFromObject(row.Get(),&rawRow);Pidl rowPidl(rawRow);
            if(!currentSource()||rowLocated!=S_OK||!rowPidl)return reject("real-search-row-pidl",rowLocated);
            if(!gate())return reject("view-row-pidl-source");
            PIDLIST_ABSOLUTE rawView=nullptr;const auto viewLocated=SHGetIDListFromObject(viewRow.Get(),&rawView);Pidl viewPidl(rawView);
            if(!currentSource()||viewLocated!=S_OK||!viewPidl||!sameBytes(rowPidl.get(),viewPidl.get())||
               !ILIsEqual(rowPidl.get(),viewPidl.get()))return reject("same-actual-view-search-row-pidl",viewLocated);
            if(!gate())return reject("repeat-child-source");
            PITEMID_CHILD rawAgain=nullptr;const auto reread=folder->Item(index,&rawAgain);Pidl childAgain(rawAgain);
            if(!currentSource()||reread!=S_OK||!sameBytes(child.get(),childAgain.get()))return reject("same-actual-view-child",reread);
            // Only the already authorized exact original native path reaches
            // file metadata. Fence each read with current source + immutable
            // original-source checks, then compare volume AND all 128 ID bits.
            if(!gate())return reject("original-before-fileid-source");
            const auto before=backing_test::identity(candidate->originalPath);
            if(before!=candidate->id||!currentSource())return reject("original-before-full-fileid");
            if(!gate())return reject("actual-fileid-source");
            const auto actualId=backing_test::identity(std::filesystem::path(rowPath.p));
            if(!currentSource())return reject("actual-fileid-stale");
            if(!gate())return reject("original-after-fileid-source");
            const auto after=backing_test::identity(candidate->originalPath);
            if(!currentSource()||before!=actualId||actualId!=after||after!=candidate->id||!actual.insert(actualId).second)
                return reject("original-before-actual-after-full-fileid-or-duplicate");
            verified.push_back({index,contextual,contextualOrder,physicalRawBytesEqual});
        }
        if(!gate())return reject("final-count-source");
        int finalCount=-1;const auto finalRead=folder->ItemCount(SVGIO_ALLVIEW,&finalCount);
        if(!currentSource()||finalRead!=S_OK||finalCount!=count)return reject("complete-final-native-count",finalRead);
        if(!gate())return reject("final-source");
        const auto verifiedAll=actual==expected&&ILIsEqual(current.get(),app.currentPidl_.get());
        if(verifiedAll&&receipts)*receipts=std::move(verified);
        return verifiedAll;
    }
    static void await(ExplorerApp& app,backing_test::Fixture& fixture,const std::wstring& query,
                      const std::vector<SearchScopeRule>& rules,const std::vector<OwnedSource>& owned,const std::set<Id>& expected) {
        const auto actionDeadline=std::min(deadline,GetTickCount64()+5000);
        while(GetTickCount64()<actionDeadline) {
            std::vector<RowReceipt> receipts;
            if(complete(app,fixture,query,rules,owned,expected,actionDeadline,false,&receipts)&&GetTickCount64()<actionDeadline) {
                static unsigned caseNumber=0;const auto number=++caseNumber;
                for(const auto& receipt:receipts)std::cout<<"AppSearch identityReceipt case="<<number<<" row="<<receipt.row
                    <<" searchContextOnlyHRESULT/order="<<static_cast<ULONG>(receipt.contextual)<<"/"<<receipt.contextualOrder
                    <<" physicalCanonicalHRESULT/order=0/0 fullPhysicalCanonicalPIDLHRESULT/order=0/0"
                    <<" fullPhysicalPIDLNativeILIsEqual=1 physicalRawBytesEqual="<<receipt.physicalRawBytesEqual
                    <<" actualViewRowPIDLRawExact=1 actualViewRowPIDLNativeILIsEqual=1"
                    <<" originalBefore/actual/originalAfterFullFileIDExact=1 originalSourcesImmutable=1\n";
                std::cout<<"AppSearch completedCase="<<number<<" nativeCount="<<receipts.size()<<" expected="<<expected.size()
                    <<" nativeCountBeforeAfterExact=1 uniqueFullFileIDs=1 within5000msAfterNativeRelease=1\n"<<std::flush;
                return;
            }
            pump(&app);Sleep(1);
        }
        // A timeout stays a failure even if the one bounded diagnostic read
        // observes late completion. This does not extend the 5000ms action gate.
        (void)complete(app,fixture,query,rules,owned,expected,actionDeadline,true);
        backing_test::require(false,"actual App query/rules/full native FileIDs did not complete within 5000ms");
    }
    static bool sameBytes(PCIDLIST_ABSOLUTE a,PCIDLIST_ABSOLUTE b) {
        if(!a||!b)return a==b;const auto size=ILGetSize(a);
        return size==ILGetSize(b)&&std::memcmp(a,b,size)==0;
    }
    static std::shared_ptr<const SearchBackingLease> currentBacking(ExplorerApp& app) {
        Pidl current(app.currentPidl_?ILCloneFull(app.currentPidl_.get()):nullptr);
        backing_test::require(current!=nullptr,"accepted native identity missing");
        const auto navigation=app.navigationCount_;
        for(const auto& entry:app.searchLocations_) {
            // Plain bytes and shared ownership only: no provider callback
            // occurs while reading an iterator into the actual App cache.
            if(entry.backing&&(sameBytes(entry.location.get(),current.get())||sameBytes(entry.completedLocation.get(),current.get())||
               sameBytes(entry.historyLocation.get(),current.get()))) {
                auto backing=entry.backing;
                backing_test::require(!app.closing_&&app.navigationCount_==navigation&&sameBytes(current.get(),app.currentPidl_.get()),"stale backing cache lookup");
                return backing;
            }
        }
        throw std::runtime_error("accepted actual location lost its typed backing owner");
    }
    static std::vector<Pidl> history(const ExplorerApp& app) {
        std::vector<Pidl> copy;copy.reserve(app.history_.size());
        for(const auto& entry:app.history_){Pidl value(entry?ILCloneFull(entry.get()):nullptr);backing_test::require(value!=nullptr,"retain native history identity");copy.push_back(std::move(value));}
        return copy;
    }
    static bool sameHistory(const ExplorerApp& app,const std::vector<Pidl>& original,int index) {
        if(app.historyIndex_!=index||app.history_.size()!=original.size())return false;
        for(size_t row=0;row<original.size();++row)if(!sameBytes(original[row].get(),app.history_[row].get()))return false;
        return true;
    }
    static void close(ComPtr<ExplorerApp>& app,backing_test::Fixture& fixture) {
        if(!app)return;
        if(app->window_)SendMessageW(app->window_,WM_CLOSE,0,0);
        const auto limit=std::min(deadline,GetTickCount64()+5000);
        while(app->window_&&GetTickCount64()<limit){pump(app.Get());Sleep(1);}
        backing_test::require(!app->window_&&SUCCEEDED(app->shutdownStatus_),"App native teardown/backing cleanup failed");
        source(fixture);app.Reset();
    }
    static void requireLayout(ExplorerApp& app,RibbonLayout requested) {
        backing_test::require(app.ribbon_.layout()==requested,"actual App ribbon layout differs from requested fixture layout");
        if(requested==RibbonLayout::InstalledWindows10)
            backing_test::exact(app.ribbon_.installedLayoutStatus(),"actual installed App ribbon layout must load without fallback");
    }
    static void run(RibbonLayout requested) {
        using namespace backing_test;
        deadline=GetTickCount64()+60000;
        Fixture fixture(true);DescriptorFiles descriptors;const auto owned=originalSources(fixture);auto shallow=rules(fixture,false,true);
        const auto query=L"System.FileName:~<\""+fixture.prefix+L"\"";
        const auto refined=L"System.FileName:=\""+fixture.directFile.filename().native()+L"\"";
        const std::set<Id> full{fixture.directId,fixture.extraId},single{fixture.directId};
        SearchWindowContext context;context.query=query;context.recursive=true;context.rules=shallow;
        context.primaryScope=shallow.front().folder;context.closeOrigin=item(fixture.scope);
        std::array<PCIDLIST_ABSOLUTE,2> scopeIds{};std::array<Pidl,2> nativeIds;
        for(size_t index=0;index<shallow.size();++index){PIDLIST_ABSOLUTE raw=nullptr;const auto hr=SHGetIDListFromObject(shallow[index].folder.Get(),&raw);nativeIds[index].reset(raw);exact(hr,"own original scope PIDLs");require(nativeIds[index]!=nullptr,"scope PIDL missing");scopeIds[index]=nativeIds[index].get();}
        exact(SHCreateShellItemArrayFromIDLists(2,scopeIds.data(),&context.scopes),"actual original ordered scope array");
        ComPtr<ExplorerApp> app;app.Attach(new ExplorerApp(GetModuleHandleW(nullptr),true,requested));
        exact(app->prepareSearchWindowContext(context),"new App prepares live backed search");
        exact(app->create(fixture.scope.native()),"create private hidden App");requireLayout(*app.Get(),requested);await(*app.Get(),fixture,query,shallow,owned,full);
        auto backing=currentBacking(*app.Get());const auto oldPath=backing->path();const auto oldId=identity(oldPath);const auto oldBytes=bytes(oldPath);
        descriptors.note(oldPath,backing->identity());descriptors.observe(*app->searchBackings_);
        const auto count=app->searchBackings_->retainedCount();backing.reset();
        exact(app->startSearch(query,true),"exact repeated query App route");await(*app.Get(),fixture,query,shallow,owned,full);
        require(app->searchBackings_->retainedCount()==count&&identity(oldPath)==oldId,"App repeat replaced backing");
        // Actual edit notification schedules the native timer route. No
        // keyboard/mouse input or direct startSearch substitutes for dispatch.
        require(SetWindowTextW(app->search_,refined.c_str())!=FALSE,"set owned native edit for debounced backed query");
        await(*app.Get(),fixture,refined,shallow,owned,single);
        require(SetWindowTextW(app->search_,query.c_str())!=FALSE,"set owned native edit to original backed query");
        await(*app.Get(),fixture,query,shallow,owned,full);
        app->setSearchText(refined);
        exact(app->execute(Search),"actual explicit Search dispatch preserves native mixed rules");await(*app.Get(),fixture,refined,shallow,owned,single);
        // Live edits intentionally replace their original travel slot. Create
        // actual direct search history entries before testing Back/Forward.
        exact(app->startSearch(query,true),"direct search retains query travel entry");await(*app.Get(),fixture,query,shallow,owned,full);
        exact(app->startSearch(refined,true),"direct refinement retains native travel entry");await(*app.Get(),fixture,refined,shallow,owned,single);
        auto refinedHistory=history(*app.Get());const auto refinedIndex=app->historyIndex_;
        exact(app->execute(Back),"actual native Back");await(*app.Get(),fixture,query,shallow,owned,full);
        require(sameHistory(*app.Get(),refinedHistory,refinedIndex-1),"Back changed original complete history identities");
        exact(app->execute(Forward),"actual native Forward");await(*app.Get(),fixture,refined,shallow,owned,single);
        require(sameHistory(*app.Get(),refinedHistory,refinedIndex),"Forward lost native history identity");
        for(const auto command:{NavigationPane,DetailsPane,PreviewPane,NavigationPane}) {
            exact(app->recreateBrowser(command),"real pane recreation/toggle");await(*app.Get(),fixture,refined,shallow,owned,single);
            require(sameHistory(*app.Get(),refinedHistory,refinedIndex)&&identity(oldPath)==oldId&&bytes(oldPath)==oldBytes,"pane recreation retired reachable backing/history");
        }
        const auto retained=app->searchBackings_->retainedCount();app->pruneSearchCaches(1);
        require(app->searchBackings_->retainedCount()==retained&&identity(oldPath)==oldId,"history pruning deleted descriptor");
        // A fresh child reconstructs query/rules, not a parent temporary path.
        SearchWindowContext childContext;exact(app->currentSearchWindowContext(&childContext),"actual current new-window packet source");
        ComPtr<ExplorerApp> child;child.Attach(new ExplorerApp(GetModuleHandleW(nullptr),true,requested));
        exact(child->prepareSearchWindowContext(childContext),"fresh child prepares own descriptor");exact(child->create(fixture.scope.native()),"create independent private child App");
        requireLayout(*child.Get(),requested);await(*child.Get(),fixture,refined,shallow,owned,single);
        auto childBacking=currentBacking(*child.Get());const auto childPath=childBacking->path();const auto childId=identity(childPath);
        descriptors.note(childPath,childBacking->identity());childBacking.reset();
        require(childPath!=oldPath&&identity(childPath)!=oldId,"new window borrowed parent backing ownership");
        close(child,fixture);require(identity(childPath)==childId&&identity(oldPath)==oldId,"child close changed child/parent persistent descriptor");
        for(const bool duringRecreation:{false,true}) {
            ComPtr<ExplorerApp> reader;reader.Attach(new ExplorerApp(GetModuleHandleW(nullptr),true,requested));
            exact(reader->prepareSearchWindowContext(childContext),"prepare independent native lifetime fixture");
            exact(reader->create(fixture.scope.native()),"create native lifetime fixture");requireLayout(*reader.Get(),requested);await(*reader.Get(),fixture,refined,shallow,owned,single);
            auto readback=currentBacking(*reader.Get());const auto readerPath=readback->path();const auto readerId=identity(readerPath);
            descriptors.note(readerPath,readback->identity());readback.reset();
            const auto readerDirectory=reader->searchBackings_->directory();unsigned closed=0;
            const auto reenterClose=[&] {
                ++closed;require(reader->searchNativeCallsActive_&&std::filesystem::exists(readerPath),"native retained-view lifetime guard missing");
                if(duringRecreation)require(!reader->browser_&&!reader->view_&&!reader->folderView_,"recreate fixture did not reach actual native teardown boundary");
                SendMessageW(reader->window_,WM_CLOSE,0,0);
                require(reader->closing_&&reader->searchClosePending_&&std::filesystem::exists(readerPath),"native retained-view close deleted backing on active stack");
            };
            if(duringRecreation) {
                reader->headlessSearchRecreateReentryProbe_=reenterClose;
                require(reader->recreateBrowser(NavigationPane)==E_ABORT,"close after real native teardown restarted Browser");
            } else {
                reader->headlessSearchReadbackReentryProbe_=reenterClose;
                SearchWindowContext obsolete;
                require(reader->currentSearchWindowContext(&obsolete)==HRESULT_FROM_WIN32(ERROR_RETRY)&&!obsolete.primaryScope,
                        "native readback close published obsolete window context");
            }
            require(closed==1&&!reader->searchNativeCallsActive_&&std::filesystem::exists(readerPath),"native lifetime callback/source release mismatch");
            close(reader,fixture);require(std::filesystem::exists(readerDirectory)&&identity(readerPath)==readerId,
                    "native readback/recreate close changed original persistent descriptor");
        }
        // Actual reentry callback advances the current interaction. The old
        // native item must not browse or overwrite the newer edit afterward.
        const auto originalNavigation=app->navigationCount_;const auto originalQuery=app->activeQuery_;
        Pidl original(ILCloneFull(app->currentPidl_.get()));unsigned reentered=0;
        app->headlessSearchFactoryReentryProbe_=[&]{++reentered;app->setSearchText(L"newer queued exact edit");app->cancelLiveSearch();};
        require(app->startSearch(query,true)==S_FALSE&&reentered==1&&app->navigationCount_==originalNavigation&&
                app->activeQuery_==originalQuery&&ILIsEqual(original.get(),app->currentPidl_.get()),"obsolete backed build overwrote native source/history");
        require(identity(oldPath)==oldId&&bytes(oldPath)==oldBytes,"cancel/reentry deleted original backing");
        // Closing inside the real construction stack is deferred until its
        // local native interfaces and lease have actually released.
        app->headlessSearchFactoryReentryProbe_=[&]{SendMessageW(app->window_,WM_CLOSE,0,0);};
        require(app->startSearch(query,true)==S_FALSE&&app->closing_&&app->searchClosePending_,"factory-close failed to defer backing deletion");
        require(std::filesystem::exists(oldPath),"factory callback deleted active backing");
        const auto directory=app->searchBackings_->directory();close(app,fixture);
        require(std::filesystem::exists(directory)&&identity(oldPath)==oldId&&bytes(oldPath)==oldBytes,
                "App close changed original persistent descriptor");
        source(fixture);original.reset();refinedHistory.clear();
        context={};childContext={};shallow.clear();nativeIds={};
        descriptors.removeExplicitFixtureFiles();fixture.remove=true;
    }
    static void runResidentStress() {
        using namespace backing_test;
        deadline=GetTickCount64()+60000;
        Fixture fixture(true);DescriptorFiles descriptors;const auto owned=originalSources(fixture);
        auto shallow=rules(fixture,false,true);const std::set<Id> expected{fixture.directId,fixture.extraId};
        const auto queryFor=[&](size_t index){return L"System.FileName:~<\""+fixture.prefix+
            L"\" AND System.Size:<="+std::to_wstring(1048576+index);};
        SearchWindowContext context;context.query=queryFor(0);context.recursive=true;context.rules=shallow;
        context.primaryScope=shallow.front().folder;context.closeOrigin=item(fixture.scope);
        std::array<PCIDLIST_ABSOLUTE,2> scopeIds{};std::array<Pidl,2> nativeIds;
        for(size_t index=0;index<shallow.size();++index) {
            PIDLIST_ABSOLUTE raw=nullptr;const auto hr=SHGetIDListFromObject(shallow[index].folder.Get(),&raw);nativeIds[index].reset(raw);
            exact(hr,"own stress original scope PIDL");require(nativeIds[index]!=nullptr,"stress scope identity missing");scopeIds[index]=nativeIds[index].get();
        }
        exact(SHCreateShellItemArrayFromIDLists(2,scopeIds.data(),&context.scopes),"actual stress original ordered scope array");
        ComPtr<ExplorerApp> app;app.Attach(new ExplorerApp(GetModuleHandleW(nullptr),true,RibbonLayout::Authored));
        exact(app->prepareSearchWindowContext(context),"actual App initial stress request");
        exact(app->create(fixture.scope.native()),"create one private App for130 lifetime requests");
        requireLayout(*app.Get(),RibbonLayout::Authored);await(*app.Get(),fixture,queryFor(0),shallow,owned,expected);
        auto first=currentBacking(*app.Get());const auto firstPath=first->path();const auto firstId=identity(firstPath);const auto firstBytes=bytes(firstPath);
        descriptors.note(firstPath,first->identity());descriptors.observe(*app->searchBackings_);
        DelayedNativeReader external;ComPtr<IShellItem> originalItem;
        exact(SHCreateItemFromIDList(app->currentPidl_.get(),IID_PPV_ARGS(&originalItem)),"retain actual first App accepted native item");
        external.capture(originalItem.Get());originalItem.Reset();first.reset();
        for(size_t index=1;index<130;++index) {
            source(fixture);exact(app->startSearch(queryFor(index),true),"actual distinct App search intent beyond old128 cap");
            await(*app.Get(),fixture,queryFor(index),shallow,owned,expected);
            require(app->searchBackings_->retainedCount()==std::min(index+1,SearchBackingStore::maximumResidentBackings),
                    "actual App Store resident cache exceeded128");
            require(app->history_.size()<=100,"actual App history exceeded existing100-entry bound");
            require(identity(firstPath)==firstId&&bytes(firstPath)==firstBytes,"actual App eviction/pruning changed old backing");
        }
        require(descriptors.files.size()==130&&app->searchBackings_->retainedCount()==128,"one actual App did not accept130 distinct native requests");
        auto travel=history(*app.Get());const auto travelIndex=app->historyIndex_;
        auto last=currentBacking(*app.Get());const auto lastPath=last->path();const auto lastId=identity(lastPath);
        exact(app->execute(Back),"actual Back after130 App search intents");await(*app.Get(),fixture,queryFor(128),shallow,owned,expected);
        require(sameHistory(*app.Get(),travel,travelIndex-1),"Back changed retained original travel PIDLs after eviction");
        exact(app->execute(Forward),"actual Forward after130 App search intents");await(*app.Get(),fixture,queryFor(129),shallow,owned,expected);
        require(sameHistory(*app.Get(),travel,travelIndex)&&identity(lastPath)==lastId,"Forward changed retained original descriptor/travel identity");
        const auto resident=app->searchBackings_->retainedCount();
        exact(app->startSearch(queryFor(129),true),"exact latest App query reuses current verified cache lease");
        await(*app.Get(),fixture,queryFor(129),shallow,owned,expected);
        require(descriptors.files.size()==130&&app->searchBackings_->retainedCount()==resident&&identity(lastPath)==lastId,
                "identical current App query allocated/replaced immutable backing");
        app->pruneSearchCaches(1);source(fixture);
        require(identity(firstPath)==firstId&&bytes(firstPath)==firstBytes,"App history metadata pruning deleted old external target");
        const auto directory=app->searchBackings_->directory();close(app,fixture);
        require(fs::exists(directory)&&identity(firstPath)==firstId&&bytes(firstPath)==firstBytes,"App close removed original external native path");
        external.verify(expected);source(fixture);descriptors.unchanged();external.release();
        // The final native history PIDLs and scope/context aliases are cleared
        // before explicit fixture cleanup; none is used as a last-user proof.
        last.reset();travel.clear();context={};shallow.clear();nativeIds={};
        descriptors.removeExplicitFixtureFiles();fixture.remove=true;
    }

};
} // namespace explorer

int wmain(int argc,wchar_t** argv) {
    auto requested=explorer::RibbonLayout::Authored;
    bool residentStress=false;
    if(argc==2&&std::wcscmp(argv[1],L"--installed")==0)requested=explorer::RibbonLayout::InstalledWindows10;
    else if(argc==2&&std::wcscmp(argv[1],L"--resident-stress")==0)residentStress=true;
    else if(argc!=1)return 2;
    const auto* layoutLabel=requested==explorer::RibbonLayout::InstalledWindows10?"installed-windows10":"authored";
    explorer::PrivateDesktop desktop;if(desktop.initialize()!=S_OK)return 3;
    explorer::NativeApartmentOwner nativeApartment;
    const auto initialized=nativeApartment.initializeOle();if(FAILED(initialized))return 4;
    int failure=0;
    try{if(residentStress)explorer::SearchBackingNativeFixture::runResidentStress();
        else explorer::SearchBackingNativeFixture::run(requested);
        std::cout<<"PASS: real App backed search mode="<<(residentStress?"130-intent-resident-stress":"original-lifetime")<<" layout="<<layoutLabel<<'\n';}
    catch(const std::exception& error){failure=1;std::cerr<<"FAIL: actual App backed search layout="<<layoutLabel<<": "<<error.what()<<'\n';}
    const auto drained=explorer::drainStaWorkers(5000);
    if(FAILED(drained)){std::cerr<<"FAIL: actual final native worker drain\n";std::cerr.flush();TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);}
    bool input=false,visible=true;
    if(desktop.verifyIsolation(&input)!=S_OK||!input||desktop.visibleWindowsOnInputDesktop(visible)!=S_OK||visible)failure=1;
    nativeApartment.finishOrTerminate();return failure;
}
