#include "explorer/native_apartment.hpp"
#include "explorer/app.hpp"
#include "explorer/worker_sta.hpp"
#include "app_shutdown_pin_accessibility.hpp"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <algorithm>

// This integration fixture supplies real, owned filesystem IShellItems to the
// production app's ordinary RecentItems source. The actual native framework
// supplies its own list and final padded pin array. Public UIA/MSAA presses its
// real pin buttons. WM_CLOSE/DestroyWindow/WM_DESTROY are production App routes.
// A private headless resolver sink stops after genuine Shell menu binding and
// enabled pintohome resolution, before any provider Invoke or pin persistence.
namespace explorer {
struct ShutdownPinsNativeFixture {
    struct InstalledUnavailable : std::runtime_error {using std::runtime_error::runtime_error;};
    enum class Reentry { None, Reset, Initialize, ReplaceRibbon, Close, DestroyOwner, ReplaceDisplayedIdentity };
    struct OwnedFolders {
        std::filesystem::path root;
        std::array<std::filesystem::path,3> paths;
        bool rootCreated=false;
        std::array<bool,3> created{};
        std::array<ComPtr<IShellItem>,3> items;
        std::array<HANDLE,3> protectedHandles{INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
        std::array<FILE_ID_INFO,3> originalIds{};
        std::array<std::wstring,3> nativePaths;
        static FILE_ID_INFO readIdentity(HANDLE handle) {
            FILE_ID_INFO identity{};
            require(GetFileInformationByHandleEx(handle,FileIdInfo,&identity,sizeof(identity))!=FALSE,
                "Read full native volume/128-bit original file identity");
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            require(GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&attributes,sizeof(attributes))!=FALSE&&
                (attributes.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),
                "Owned original directory became a reparse or a different object type");
            return identity;
        }
        static bool sameIdentity(const FILE_ID_INFO& left,const FILE_ID_INFO& right) {
            return left.VolumeSerialNumber==right.VolumeSerialNumber&&
                std::memcmp(left.FileId.Identifier,right.FileId.Identifier,sizeof(left.FileId.Identifier))==0;
        }
        static std::wstring nativePath(IShellItem* item) {
            PWSTR raw=nullptr;succeeded(item->GetDisplayName(SIGDN_FILESYSPATH,&raw),"Read resolved actual Shell filesystem identity path");
            struct Text {PWSTR value;~Text(){CoTaskMemFree(value);}} text{raw};
            require(text.value&&*text.value,"Resolved native Shell filesystem path is empty");
            return text.value;
        }
        void verify(unsigned index,IShellItem* actual=nullptr) {
            require(index<items.size()&&protectedHandles[index]!=INVALID_HANDLE_VALUE,"Original protected source is absent");
            require(sameIdentity(originalIds[index],readIdentity(protectedHandles[index])),"Original retained native file handle identity changed");
            if(!actual)actual=items[index].Get();
            const auto path=nativePath(actual);
            require(path==nativePaths[index],"Actual resolved Shell item changed original native path spelling");
            int order=1;const auto compared=actual->Compare(items[index].Get(),SICHINT_CANONICAL,&order);
            require(compared==S_OK&&order==0,"Actual bound item differs canonically from its original Shell target");
            const auto read=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            require(read!=INVALID_HANDLE_VALUE,"Open resolved native path under original protection");
            struct Handle {HANDLE value;~Handle(){CloseHandle(value);}} current{read};
            require(sameIdentity(originalIds[index],readIdentity(current.value)),"Resolved native path is a different full volume/file ID");
        }
        void finish() {
            for(unsigned index=0;index<3;++index)verify(index);
            for(auto& item:items){item.Reset();checkOwnedPinDeadline();}
            for(auto& handle:protectedHandles)if(handle!=INVALID_HANDLE_VALUE) {
                const auto old=handle;handle=INVALID_HANDLE_VALUE;
                require(CloseHandle(old)!=FALSE,"Release exact original protected source handle");
            }
            for(size_t index=0;index<paths.size();++index) {
                require(created[index]&&std::filesystem::remove(paths[index]),"Remove exact empty owned original source directory");
                created[index]=false;paths[index].clear();
            }
            require(rootCreated&&std::filesystem::remove(root),"Remove exact empty owned fixture root");rootCreated=false;root.clear();
            checkOwnedPinDeadline();
        }
        OwnedFolders() {
            try {
            wchar_t temp[MAX_PATH+1]{};const auto tempLength=GetTempPathW(MAX_PATH,temp);
            require(tempLength>0&&tempLength<MAX_PATH&&temp[0],"Read bounded absolute owned fixture parent");
            GUID identity{};succeeded(CoCreateGuid(&identity),"Create unique owned folder identity");
            wchar_t text[40]{};require(StringFromGUID2(identity,text,40)>0,"Format unique fixture identity");
            root=std::filesystem::path(temp)/(std::wstring(L"ExplorerShutdownPins-")+text);
            require(root.is_absolute(),"Owned fixture parent is not absolute");
            rootCreated=std::filesystem::create_directory(root);
            require(rootCreated,"Create new owned fixture root");
            for(size_t index=0;index<paths.size();++index) {
                paths[index]=root/(L"OriginalShellFolder"+std::to_wstring(index));
                created[index]=std::filesystem::create_directory(paths[index]);
                require(created[index],"Create new owned Shell folder");
                succeeded(SHCreateItemFromParsingName(paths[index].c_str(),nullptr,IID_PPV_ARGS(&items[index])),
                    "Create original real native Shell item");
                nativePaths[index]=nativePath(items[index].Get());
                protectedHandles[index]=CreateFileW(nativePaths[index].c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,
                    OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                require(protectedHandles[index]!=INVALID_HANDLE_VALUE,"Retain read-only source handle denying writes/delete");
                originalIds[index]=readIdentity(protectedHandles[index]);verify(static_cast<unsigned>(index));
            }
            }catch(...){cleanup();throw;}
        }
        ~OwnedFolders() {cleanup();}
        void cleanup() noexcept {
            for(auto& item:items)item.Reset();
            for(auto& handle:protectedHandles)if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}
            // Each path was created exclusively above. Never remove recursively
            // or delete pre-existing paths if an unexpected native file appears.
            std::error_code error;
            for(size_t index=0;index<paths.size();++index)if(created[index])std::filesystem::remove(paths[index],error);
            if(rootCreated)std::filesystem::remove(root,error);
        }
    };
    static void pump() {
        MSG message{};
        for(unsigned count=0;count<256&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++count) {
            checkOwnedPinDeadline();
            require(message.message!=WM_QUIT,"Headless app posted unexpected process quit");
            TranslateMessage(&message);DispatchMessageW(&message);checkOwnedPinDeadline();
        }
        MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        checkOwnedPinDeadline();
    }
    static void ready(ExplorerApp& app) {
        const auto end=ownedPinDeadline;
        while(!app.view_||!app.folderView_||app.navigating_||app.commandRefreshActive_||app.commandStatesPending()) {
            require(GetTickCount64()<end,"Actual owned app did not settle native view/command state");pump();
        }
        require(app.window_&&IsWindow(app.window_)&&!app.closing_,"Original app host is not live");
    }
    static void printFacts(const char* label,const ExplorerApp::HeadlessShutdownPinFacts& facts) {
        std::cout<<label<<"{epoch="<<facts.epoch<<",close="<<facts.closeEntry<<",display="<<facts.displayRevision
            <<",generation="<<facts.generation<<",navigation="<<facts.navigation<<",rows="<<facts.displayedCount
            <<",closing="<<facts.closing<<",destroying="<<facts.destroying<<",navigating="<<facts.navigating
            <<",commandRefresh="<<facts.commandRefresh<<",window="<<facts.window<<",browser="<<facts.browser
            <<",view="<<facts.view<<",folder="<<facts.folder<<",location="<<facts.location<<",transaction="<<facts.transaction
            <<",hookGeneration="<<facts.hookGeneration<<",ownedCloseEntryTick="<<facts.ownedCloseEntryTick
            <<",hookAttached="<<facts.hookAttached<<",hookTop="<<facts.hookTop<<",closeFrame="<<facts.closeFrame
            <<",originalClose="<<facts.originalClose<<",originalReset="<<facts.originalReset<<",sourceRevoked="<<facts.sourceRevoked<<"}";
    }
    struct PinStageSnapshot {
        const char* label=nullptr;
        ULONGLONG tick=0;
        UINT nativeCount=0,normalCount=0;
        bool nativeOverflow=false,normalOverflow=false;
        ExplorerApp::HeadlessShutdownPinFacts facts;
    };
    static PinStageSnapshot snapshot(const char* label,ExplorerApp& app) {
        PinStageSnapshot stage;stage.label=label;stage.tick=GetTickCount64();
        stage.facts=app.headlessShutdownPinFacts();
        RibbonRecentItemsDiagnostics native;app.ribbon_.recentItemsDiagnostics(native);
        stage.nativeCount=native.count;stage.nativeOverflow=native.overflow;
        stage.normalCount=app.headlessNormalPinResultCount_;stage.normalOverflow=app.headlessNormalPinResultOverflow_;
        return stage;
    }
    static void printTiming(const std::array<PinStageSnapshot,4>& stages,ExplorerApp& app) {
        for(const auto& stage:stages) {
            std::cout<<"DIAGNOSTIC timing "<<stage.label<<" tick="<<stage.tick
                <<" nativeCount="<<stage.nativeCount<<" normalCount="<<stage.normalCount
                <<" nativeOverflow="<<stage.nativeOverflow<<" normalOverflow="<<stage.normalOverflow<<" ";
            printFacts("state",stage.facts);std::cout<<"\n";
        }
        std::cout<<"DIAGNOSTIC normalApp callbacks="<<app.headlessNormalPinResultCount_
            <<" overflow="<<app.headlessNormalPinResultOverflow_<<" phases=1:action,2:collection,3:ordinary-deny,4:preClose,5:close\n";
        for(UINT index=0;index<std::min<UINT>(app.headlessNormalPinResultCount_,static_cast<UINT>(app.headlessNormalPinResults_.size()));++index) {
            const auto& result=app.headlessNormalPinResults_[index];
            std::cout<<"DIAGNOSTIC normalApp #"<<index<<" row="<<result.index<<" pinned="<<result.pinned
                <<" phase="<<result.phase<<" tick="<<result.entryTick<<"->"<<result.returnTick
                <<" hr=0x"<<std::hex<<static_cast<unsigned long>(result.result)<<std::dec<<" ";
            printFacts("entry",result.entry);std::cout<<" ";printFacts("postReleaseReturn",result.returned);std::cout<<"\n";
        }
    }
    static void printDiagnostics(const char* label,ExplorerApp& app,const std::vector<UINT>& resolved,
        const ExplorerApp::HeadlessShutdownPinFacts& preClose,UINT sourceRequests) {
        std::cout<<"DIAGNOSTIC "<<label<<" resolved=[";
        for(const auto index:resolved)std::cout<<index<<",";
        std::cout<<"] sourceRequests="<<sourceRequests<<" admission="<<app.headlessShutdownPinAdmission_<<" ";
        printFacts("preClose",preClose);std::cout<<" ";printFacts("resetEntry",app.headlessShutdownPinAdmissionFacts_);std::cout<<"\n";
        std::cout<<"DIAGNOSTIC original close continuation lowerCalls="<<app.headlessShutdownOriginalLowerCalls_
            <<" plainClassCalls="<<app.headlessShutdownPlainCloseCalls_<<" goneCalls="<<app.headlessShutdownGoneCloseCalls_
            <<" deferredSameCloseRequests="<<app.headlessShutdownDeferredCloseCalls_
            <<" lastDispatchTick="<<app.headlessShutdownDispatchTick_<<" savedLower="<<app.headlessShutdownSavedLower_
            <<" actualReceiver="<<app.headlessShutdownActualReceiver_<<"\n";
        const auto printCloseState=[](const char* name,const ExplorerApp::HeadlessOwnedCloseState& state) {
            std::cout<<name<<"{original="<<state.original<<",forwarded="<<state.forwarded<<",destroyForwarded="<<state.destroyForwarded
                <<",hookAttached="<<state.hookAttached<<",hookRetired="<<state.hookRetired
                <<",frameGeneration="<<state.frameGeneration<<",hookGeneration="<<state.hookGeneration
                <<",originalWindow="<<state.originalWindow<<",currentWindow="<<state.currentWindow<<",ownedWindow="<<state.ownedWindow
                <<",originalFrame="<<state.originalFrame<<",activeFrame="<<state.activeFrame
                <<",creator="<<state.creator<<",process="<<state.process<<"}";
        };
        std::cout<<"DIAGNOSTIC first native close entered="<<app.headlessShutdownNativeCloseEntered_
            <<" returned="<<app.headlessShutdownNativeCloseReturned_<<" caughtAfterDispatch="<<app.headlessShutdownCaughtAfterDispatch_
            <<" postLowerGateReached="<<app.headlessShutdownPostLowerGateReached_
            <<" evaluatedMask="<<app.headlessShutdownPostLowerGateEvaluated_<<" passedMask="<<app.headlessShutdownPostLowerGatePassed_<<" ";
        printCloseState("postLower",app.headlessShutdownPostLowerState_);std::cout<<" ";
        printCloseState("caught",app.headlessShutdownCaughtState_);std::cout<<"\n";
        // Bit order: capturedCurrent,frameOriginal,notForwarded,activeFrame,
        // windowSame,hookGenerationSame,ownedWindowSame,creator,IsWindow,
        // windowCreator,process,userData. Unevaluated bits have no native result.
        std::cout<<"DIAGNOSTIC actual missing original window reconciliation count="<<app.headlessShutdownMissingWindowDestructions_
            <<" cleanupStarted="<<app.windowDestructionCleanupStarted_
            <<" cleanupCompleted="<<app.headlessShutdownMissingWindowCleanupCompleted_<<"\n";
        std::cout<<"DIAGNOSTIC app callbacks="<<app.headlessShutdownPinResultCount_
            <<" overflow="<<app.headlessShutdownPinResultOverflow_<<"\n";
        for(UINT index=0;index<std::min<UINT>(app.headlessShutdownPinResultCount_,static_cast<UINT>(app.headlessShutdownPinResults_.size()));++index) {
            const auto& result=app.headlessShutdownPinResults_[index];
            std::cout<<"DIAGNOSTIC app row="<<result.index<<" pinned="<<result.pinned<<" hr=0x"<<std::hex
                <<static_cast<unsigned long>(result.result)<<std::dec<<" ";
            printFacts("entry",result.entry);std::cout<<" ";printFacts("postReleaseReturn",result.returned);std::cout<<"\n";
        }
        RibbonRecentItemsDiagnostics native;app.ribbon_.recentItemsDiagnostics(native);
        std::cout<<"DIAGNOSTIC native receipts="<<native.count<<" overflow="<<native.overflow<<"\n";
        for(UINT index=0;index<std::min<UINT>(native.count,static_cast<UINT>(native.receipts.size()));++index) {
            const auto& receipt=native.receipts[index];
            const auto kind=receipt.kind==RibbonRecentItemsReceiptKind::Source?"source":
                receipt.kind==RibbonRecentItemsReceiptKind::NormalCommit?"normalCommit":
                receipt.kind==RibbonRecentItemsReceiptKind::NormalPinned?"normalPinned":
                receipt.kind==RibbonRecentItemsReceiptKind::RetiredCommit?"retiredCommit":"reset";
            std::cout<<"DIAGNOSTIC native #"<<index<<" "<<kind<<" tick="<<receipt.entryTick<<"->"<<receipt.exitTick
                <<" epoch="<<receipt.entryEpoch<<"->"<<receipt.exitEpoch
                <<" revision="<<receipt.entryRevision<<"->"<<receipt.exitRevision
                <<" retired="<<receipt.retired<<"->"<<receipt.exitRetired
                <<" windowDestroyed="<<receipt.windowDestroyed<<"->"<<receipt.exitWindowDestroyed
                <<" recentKey="<<receipt.recentItemsKey<<" override="<<receipt.finalOverride
                <<" shutdownActive="<<receipt.shutdownActive<<" started="<<receipt.shutdownStarted
                <<" vt="<<receipt.variantType<<" bounds="<<receipt.first<<":"<<receipt.last
                <<" initial="<<receipt.initialCount<<" decoded="<<receipt.decodedCount<<" callbacks="<<receipt.callbackCount
                <<" requested="<<receipt.requestedIndex<<":"<<receipt.requestedPin
                <<" hr=0x"<<std::hex<<static_cast<unsigned long>(receipt.result)<<std::dec<<" rows=[";
            for(UINT row=0;row<std::min<UINT>(receipt.initialCount,64);++row) {
                std::cout<<row<<":"<<receipt.initialPins[row]<<"->";
                if(receipt.decoded[row])std::cout<<receipt.decodedPins[row];else std::cout<<"?";
                if(receipt.dispatched[row])std::cout<<"/0x"<<std::hex<<static_cast<unsigned long>(receipt.callbackResults[row])<<std::dec
                    <<"@"<<receipt.callbackReturnTicks[row];
                std::cout<<",";
            }
            std::cout<<"]\n";
        }
        std::cout.flush();
    }
    static void runCase(const PrivateDesktop& desktop,RibbonLayout layout,OwnedFolders& folders,
        unsigned presses,bool bothRows,bool optIn,Reentry reentry,const char* label,ULONGLONG sharedDeadline) {
        ownedPinDeadline=std::min(sharedDeadline,GetTickCount64()+18000);
        for(unsigned index=0;index<3;++index)folders.verify(index);
        ComPtr<ExplorerApp> app;app.Attach(new ExplorerApp(GetModuleHandleW(nullptr),true,layout));
        succeeded(app->create(folders.root.native()),"Create actual production owned headless App");
        if(layout==RibbonLayout::InstalledWindows10&&app->ribbon_.layout()!=layout)
            throw InstalledUnavailable("Installed native Windows 10 Ribbon unavailable; authored fallback is not installed evidence");
        ready(*app.Get());
        require(app->ribbon_.layout()==layout,"App silently changed requested native Ribbon layout");
        if(layout==RibbonLayout::InstalledWindows10)
            require(app->ribbon_.installedLayoutStatus()==S_OK,"Requested installed native Ribbon failed");
        succeeded(app->ribbon_.enableRecentItemsDiagnostics(),"Enable fixed actual native RecentItems receipts");
        app->cancelFrequentPlaces();
        app->frequentPlaces_={{folders.items[0],L"Owned Recent One",folders.paths[0].native(),false},
                              {folders.items[1],L"Owned Recent Two",folders.paths[1].native(),false}};
        app->frequentPlacesReadAt_=GetTickCount64();
        const auto fixtureSourceRevision=app->displayedFrequentPlacesRevision_;
        succeeded(app->ribbon_.invalidate(RibbonFrequentPlaces),"Publish owned real Shell RecentItems source");
        succeeded(app->ribbon_.flush(),"Flush actual app Ribbon source");
        const auto originalWindow=app->window_;const auto originalThread=GetCurrentThreadId();
        ComPtr<IShellView> originalView=app->view_;
        ComPtr<IFolderView2> originalFolder=app->folderView_;
        const auto generation=app->namespaceGeneration_;const auto navigation=app->navigationCount_;
        const auto originalEpoch=app->ribbon_.callbackEntryEpoch();
        const auto originalFramework=app->ribbon_.nativeFramework();
        Window replacement;
        if(reentry==Reentry::ReplaceRibbon) {
            replacement.handle=CreateWindowExW(0,L"WindowsExplorerOwnedShutdownPinReplacement",L"Owned replacement Ribbon",
                WS_OVERLAPPEDWINDOW,0,0,900,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(replacement.handle!=nullptr,"Create separate owned replacement framework host");
        }
        std::vector<UINT> resolvedIndices;
        unsigned reentries=0;
        std::exception_ptr sinkFailure;
        if(optIn)app->headlessShutdownPinResolved_=[&](UINT index,IShellItem* item,IShellView* site,
            bool pinned,const ContextMenuEntry& entry)->HRESULT {
            try {
            require(app->closing_&&app->shutdownPinTransaction_,"Resolution bypassed actual App shutdown gate");
            require(GetCurrentThreadId()==originalThread&&app->window_==originalWindow&&IsWindow(originalWindow),
                "Original creator STA/HWND did not survive native Destroy resolution");
            require(app->view_.Get()==originalView.Get()&&app->folderView_.Get()==originalFolder.Get()&&
                site==originalView.Get()&&app->namespaceGeneration_==generation&&app->navigationCount_==navigation,
                "Final pin resolution lost original App view/generation/navigation");
            require(index<2&&pinned&&entry.id&&entry.enabled()&&
                !entry.submenu&&entry.canonicalVerb==L"pintohome","Final pin selected a different original Shell target/leaf");
            folders.verify(index,item); // the actual array item used to bind the genuine menu
            require(app->pinFrequentPlace(index,true)==E_ABORT&&app->execute(Copy)==E_ABORT&&
                !app->frequentPlacesTask_,"Closing admitted ordinary commands or source refresh");
            resolvedIndices.push_back(index);
            if(reentry!=Reentry::None&&reentries++==0) {
                switch(reentry) {
                case Reentry::Reset: app->ribbon_.reset();break;
                case Reentry::Initialize: {
                    RibbonCallbacks callbacks;
                    require(app->ribbon_.initialize(nullptr,GetModuleHandleW(nullptr),std::move(callbacks),layout)==E_INVALIDARG,
                        "Actual invalid initialize entry did not reject its absent host");break;
                }
                case Reentry::ReplaceRibbon: {
                    // The genuine final batch can precede App WM_DESTROY.
                    // Retire the actual old framework before creating a real
                    // replacement; the ordinary initialize guard stays strict.
                    app->ribbon_.reset();
                    RibbonCallbacks callbacks;
                    callbacks.query=[](UINT){return RibbonCommandState{};};
                    callbacks.execute=[](UINT){return E_ACCESSDENIED;};
                    callbacks.executeItem=[](UINT,UINT){return E_ACCESSDENIED;};
                    callbacks.pinItem=[](UINT,bool){return E_ACCESSDENIED;};
                    callbacks.items=[](UINT){return std::vector<RibbonItem>{};};
                    succeeded(app->ribbon_.initialize(replacement.handle,GetModuleHandleW(nullptr),std::move(callbacks),layout),
                        "Actual successful newer native framework initialization");
                    require(app->ribbon_.valid()&&app->ribbon_.layout()==layout&&app->ribbon_.nativeFramework()!=originalFramework,
                        "Actual successful replacement did not publish a distinct native framework");break;
                }
                case Reentry::Close: SendMessageW(originalWindow,WM_CLOSE,0,0);break;
                case Reentry::DestroyOwner:
                    require(DestroyWindow(originalWindow)!=FALSE,"Actual nested owner DestroyWindow was refused");
                    require(!IsWindow(originalWindow)&&!app->window_,"Actual nested owner destruction did not reach native NCDESTROY");break;
                case Reentry::ReplaceDisplayedIdentity:
                    // Deliberately replace only the old native index with a real
                    // third Shell item. The actual old pin array must not follow.
                    app->displayedFrequentPlaces_[1].item=folders.items[2];break;
                case Reentry::None: break;
                }
            }
            return S_OK; // resolved only; production acceptance follows the release/source fence
            }catch(...) {
                if(!sinkFailure)sinkFailure=std::current_exception();
                return E_FAIL; // preserve fixture failure across COM/production catches
            }
        };
        require(SetWindowPos(originalWindow,nullptr,0,0,0,0,SWP_SHOWWINDOW|SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER)!=FALSE,
            "Realize original app only on its owned private desktop");
        WINDOWPLACEMENT placement{sizeof(placement)};RECT client{};
        require(GetWindowPlacement(originalWindow,&placement)!=FALSE&&GetClientRect(originalWindow,&client)!=FALSE&&
            (GetWindowLongPtrW(originalWindow,GWL_STYLE)&WS_VISIBLE)&&IsWindowVisible(originalWindow)&&
            placement.showCmd!=SW_HIDE&&placement.showCmd!=SW_SHOWMINIMIZED&&client.right>client.left&&client.bottom>client.top,
            "Actual owned app realization/style/showCmd/client readback failed");
        UpdateWindow(originalWindow);checkOwnedPinDeadline();
        app->headlessNormalPinDiagnostics_=true;app->headlessNormalPinPhase_=1;
        std::array<PinStageSnapshot,4> stages;
        auto action=std::async(std::launch::async,openAndPress,GetThreadDesktop(originalThread),
            originalThread,false,presses,bothRows,originalWindow,ownedPinDeadline);
        pumpUntil(action,ownedPinDeadline);const auto native=action.get();checkOwnedPinDeadline();
        stages[0]=snapshot("after-action-get",*app.Get());app->headlessNormalPinPhase_=2;
        succeeded(native.result,"Press actual production App native pin controls");
        require(native.opened&&native.presses==presses*(bothRows?2u:1u)&&
            native.finalPressed==((presses&1u)!=0)&&(!bothRows||native.secondFinalPressed==((presses&1u)!=0)),
            "Actual native App pin state did not match physical accessibility actions");
        require(resolvedIndices.empty(),"App shutdown sink was called before actual Close/Destroy boundary");
        RibbonCollectionReadback source;
        succeeded(app->ribbon_.collectionReadback(RibbonFrequentPlaces,source),"Read genuine app RecentItems publication receipt");
        stages[1]=snapshot("after-collection-readback",*app.Get());app->headlessNormalPinPhase_=3;
        require(source.registered&&app->displayedFrequentPlacesRevision_>fixtureSourceRevision&&app->displayedFrequentPlaces_.size()==2&&
            app->displayedFrequentPlaces_[0].item.Get()==folders.items[0].Get()&&
            app->displayedFrequentPlaces_[1].item.Get()==folders.items[1].Get(),"Native app UI did not publish original real Shell source");
        require(app->pinFrequentPlace(0,true)==E_ACCESSDENIED,"Ordinary headless profile pin unexpectedly accepted");
        stages[2]=snapshot("after-ordinary-deny-check",*app.Get());app->headlessNormalPinPhase_=4;
        // No synthetic Execute or manufactured pin SAFEARRAY is used anywhere.
        // Real window destruction causes the framework's actual final batch.
        const auto preClose=app->headlessShutdownPinFacts();
        stages[3]=snapshot("before-real-WM_CLOSE",*app.Get());app->headlessNormalPinPhase_=5;
        SendMessageW(originalWindow,WM_CLOSE,0,0);
        checkOwnedPinDeadline();
        printTiming(stages,*app.Get());
        printDiagnostics(label,*app.Get(),resolvedIndices,preClose,source.sourceRequests);
        checkOwnedPinDeadline();
        if(sinkFailure)std::rethrow_exception(sinkFailure);
        require(!app->window_&&!IsWindow(originalWindow)&&app->closing_&&app->shutdownStatus_==S_OK&&
            app->ribbon_.valid()==(reentry==Reentry::ReplaceRibbon)&&!app->browser_&&!app->view_&&!app->folderView_&&
            !app->shutdownPinTransaction_&&!app->frequentPlacesTask_,"Actual App WM_DESTROY/WM_NCDESTROY cleanup did not finish");
        require(app->ribbon_.callbackEntryEpoch()>originalEpoch,"Original native Destroy did not retire its callback binding");
        const auto changed=optIn&&(presses&1u)!=0;
        const auto expected=changed?(reentry==Reentry::None?(bothRows?std::vector<UINT>{0,1}:std::vector<UINT>{0}):std::vector<UINT>{0}):std::vector<UINT>{};
        require(resolvedIndices==expected,"Actual final App pin transaction resolved unchanged/stale/padding/wrong rows");
        const auto callbacks=(presses&1u)==0?0u:
            (reentry==Reentry::Reset||reentry==Reentry::Initialize||reentry==Reentry::ReplaceRibbon||reentry==Reentry::DestroyOwner?1u:(bothRows?2u:1u));
        require(!app->headlessShutdownPinResultOverflow_&&app->headlessShutdownPinResultCount_==callbacks,
            "Actual final production callback-return count differs from the genuine changed native rows");
        UINT productionAccepted=0;
        for(UINT index=0;index<callbacks;++index) {
            const auto expectedResult=!optIn?E_ACCESSDENIED:
                (reentry==Reentry::None?S_OK:(index==0?HRESULT_FROM_WIN32(ERROR_RETRY):E_ABORT));
            require(app->headlessShutdownPinResults_[index].index==index&&
                app->headlessShutdownPinResults_[index].result==expectedResult,
                "Actual final production callback did not accept/deny/revoke after native binding release");
            if(app->headlessShutdownPinResults_[index].result==S_OK)++productionAccepted;
        }
        require(reentry==Reentry::None||reentries==1,"Reentered final pin scope continued after revocation");
        app->ribbon_.reset();require(resolvedIndices==expected&&!app->ribbon_.valid()&&app->headlessShutdownPinResultCount_==callbacks,
            "Completed App final pin batch replayed after close");
        app->headlessShutdownPinResolved_={};originalFolder.Reset();checkOwnedPinDeadline();
        originalView.Reset();checkOwnedPinDeadline();app.Reset();checkOwnedPinDeadline();
        if(replacement.handle){require(DestroyWindow(replacement.handle)!=FALSE,"Destroy exact owned replacement host");replacement.handle=nullptr;}
        for(unsigned index=0;index<3;++index)folders.verify(index);
        bool isolated=false,visible=true;
        succeeded(desktop.verifyIsolation(&isolated),"Verify private desktop after actual App close");
        succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Read input desktop visibility after App close");
        require(isolated&&!visible,"Actual App pin-close regression exposed input desktop UI");
        std::cout<<"Observed: "<<label<<" (resolved="<<resolvedIndices.size()<<"; productionAccepted="<<productionAccepted
            <<"; providerInvoke=0; persistence=unproved)\n";
    }
    // Controlled fixture reentry before the first source retain. This does
    // not claim a Shell provider naturally pumped inside its AddRef.
    static void runCaptureBoundaryCase(const PrivateDesktop& desktop,RibbonLayout layout,OwnedFolders& folders,
        bool destroy,const char* label,ULONGLONG sharedDeadline) {
        ownedPinDeadline=std::min(sharedDeadline,GetTickCount64()+18000);
        for(unsigned index=0;index<3;++index)folders.verify(index);
        ComPtr<ExplorerApp> app;app.Attach(new ExplorerApp(GetModuleHandleW(nullptr),true,layout));
        succeeded(app->create(folders.root.native()),"Create actual production owned headless App");
        if(layout==RibbonLayout::InstalledWindows10&&app->ribbon_.layout()!=layout)
            throw InstalledUnavailable("Installed native Windows 10 Ribbon unavailable; authored fallback is not installed evidence");
        ready(*app.Get());
        require(app->ribbon_.layout()==layout,"App silently changed requested native Ribbon layout");
        if(layout==RibbonLayout::InstalledWindows10)
            require(app->ribbon_.installedLayoutStatus()==S_OK,"Requested installed native Ribbon failed");
        succeeded(app->ribbon_.enableRecentItemsDiagnostics(),"Enable fixed actual native RecentItems receipts");
        app->cancelFrequentPlaces();
        app->frequentPlaces_={{folders.items[0],L"Owned Recent One",folders.paths[0].native(),false},
                              {folders.items[1],L"Owned Recent Two",folders.paths[1].native(),false}};
        app->frequentPlacesReadAt_=GetTickCount64();
        const auto fixtureSourceRevision=app->displayedFrequentPlacesRevision_;
        succeeded(app->ribbon_.invalidate(RibbonFrequentPlaces),"Publish owned real Shell RecentItems source");
        succeeded(app->ribbon_.flush(),"Flush actual app Ribbon source");
        const auto originalWindow=app->window_;const auto originalThread=GetCurrentThreadId();
        ComPtr<IShellView> originalView=app->view_;
        ComPtr<IFolderView2> originalFolder=app->folderView_;
        const auto generation=app->namespaceGeneration_;const auto navigation=app->navigationCount_;
        const auto originalEpoch=app->ribbon_.callbackEntryEpoch();
        const auto originalFramework=app->ribbon_.nativeFramework();
        require(SetWindowPos(originalWindow,nullptr,0,0,0,0,SWP_SHOWWINDOW|SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER)!=FALSE,
            "Realize original app only on its owned private desktop");
        WINDOWPLACEMENT placement{sizeof(placement)};RECT client{};
        require(GetWindowPlacement(originalWindow,&placement)!=FALSE&&GetClientRect(originalWindow,&client)!=FALSE&&
            (GetWindowLongPtrW(originalWindow,GWL_STYLE)&WS_VISIBLE)&&IsWindowVisible(originalWindow)&&
            placement.showCmd!=SW_HIDE&&placement.showCmd!=SW_SHOWMINIMIZED&&client.right>client.left&&client.bottom>client.top,
            "Actual owned app realization/style/showCmd/client readback failed");
        UpdateWindow(originalWindow);checkOwnedPinDeadline();
        std::vector<UINT> resolved;
        app->headlessShutdownPinResolved_=[&](UINT index,IShellItem*,IShellView*,bool,const ContextMenuEntry&)->HRESULT {
            resolved.push_back(index);return E_UNEXPECTED;
        };
        auto action=std::async(std::launch::async,openAndPress,GetThreadDesktop(originalThread),
            originalThread,false,1u,false,originalWindow,ownedPinDeadline);
        pumpUntil(action,ownedPinDeadline);const auto native=action.get();checkOwnedPinDeadline();
        succeeded(native.result,"Press original real native pin before retain-boundary control");
        require(native.opened&&native.presses==1&&native.finalPressed,
            "Retain-boundary case did not physically press the actual original native pin");
        RibbonCollectionReadback source;
        succeeded(app->ribbon_.collectionReadback(RibbonFrequentPlaces,source),"Read original native retain-boundary publication");checkOwnedPinDeadline();
        RibbonRecentItemsDiagnostics published;app->ribbon_.recentItemsDiagnostics(published);
        require(!published.overflow&&published.count<=published.receipts.size(),"Original native publication receipts overflowed");
        const auto actualRecentItemsSourceCount=std::count_if(published.receipts.begin(),published.receipts.begin()+published.count,
            [](const auto& row){return row.kind==RibbonRecentItemsReceiptKind::Source&&row.recentItemsKey&&
                row.initialCount==2&&!row.initialPins[0]&&!row.initialPins[1]&&row.result==S_OK;});
        require(source.registered&&actualRecentItemsSourceCount>0&&!published.overflow&&
            app->displayedFrequentPlacesRevision_>fixtureSourceRevision&&app->displayedFrequentPlaces_.size()==2&&
            app->displayedFrequentPlaces_[0].item.Get()==folders.items[0].Get()&&
            app->displayedFrequentPlaces_[1].item.Get()==folders.items[1].Get(),
            "Retain-boundary case lost actual original RecentItems/source identity");
        for(unsigned index=0;index<3;++index){folders.verify(index);checkOwnedPinDeadline();}
        require(app->pinFrequentPlace(0,true)==E_ACCESSDENIED&&resolved.empty(),
            "Retain-boundary ordinary headless profile pin escaped denial");checkOwnedPinDeadline();
        unsigned probes=0;
        ULONGLONG probeTick=0,probeReturnTick=0;
        ExplorerApp::HeadlessShutdownPinFacts beforeProbe{},afterProbe{};
        RibbonRecentItemsDiagnostics beforeNative{},afterNative{};
        std::exception_ptr probeFailure;
        app->headlessBeforeShutdownPinRetain_=[&] {
            try {
                ++probes;probeTick=GetTickCount64();beforeProbe=app->headlessShutdownPinFacts();
                app->ribbon_.recentItemsDiagnostics(beforeNative);
                require(probes==1&&GetCurrentThreadId()==originalThread&&app->window_==originalWindow&&
                    beforeProbe.closing&&beforeProbe.transaction&&beforeProbe.capturing&&beforeProbe.nativeCloseScope&&
                    beforeProbe.hookAttached&&beforeProbe.hookTop&&beforeProbe.originalClose&&
                    !beforeProbe.originalReset&&!beforeProbe.sourceRevoked&&app->headlessShutdownPinRetains_==0&&
                    beforeProbe.generation==generation&&beforeProbe.navigation==navigation&&
                    beforeProbe.epoch==originalEpoch&&beforeProbe.displayedCount==2&&
                    app->ribbon_.nativeFramework()==originalFramework&&app->view_.Get()==originalView.Get()&&
                    app->folderView_.Get()==originalFolder.Get(),
                    "Controlled first-retain probe did not enter the armed original capture before provider AddRef");
                // No source/provider read is inserted at this boundary. Facts
                // and native receipts above are plain fixed observations.
                checkOwnedPinDeadline();
                if(destroy) {
                    require(DestroyWindow(originalWindow)!=FALSE,"Controlled first-retain original DestroyWindow failed");checkOwnedPinDeadline();
                    require(!IsWindow(originalWindow)&&!app->window_,"Controlled first-retain destroy missed real NCDESTROY");
                } else {
                    app->ribbon_.reset();checkOwnedPinDeadline();
                    require(!app->ribbon_.valid(),"Controlled first-retain real reset failed to retire original framework");
                }
                afterProbe=app->headlessShutdownPinFacts();app->ribbon_.recentItemsDiagnostics(afterNative);
                probeReturnTick=GetTickCount64();checkOwnedPinDeadline();
            }catch(...) {if(!probeFailure)probeFailure=std::current_exception();}
        };
        const auto preClose=app->headlessShutdownPinFacts();checkOwnedPinDeadline();
        SendMessageW(originalWindow,WM_CLOSE,0,0);checkOwnedPinDeadline();
        printDiagnostics(label,*app.Get(),resolved,preClose,source.sourceRequests);
        std::cout<<"DIAGNOSTIC controlled-before-first-source-AddRef mode="<<(destroy?"DestroyWindow":"NativeRibbon.reset")
            <<" tick="<<probeTick<<"->"<<probeReturnTick<<" dispatchTick="<<app->headlessShutdownDispatchTick_
            <<" nativeReceipts="<<beforeNative.count<<"->"<<afterNative.count
            <<" captureScope="<<beforeProbe.capturing<<":"<<beforeProbe.nativeCloseScope
            <<" captureDenied="<<app->headlessShutdownCaptureDenied_<<" captureReady="<<app->headlessShutdownCaptureReady_
            <<" ItemsSourceCount="<<source.sourceRequests<<" actualRecentItemsSourceCount="<<actualRecentItemsSourceCount
            <<" retains="<<app->headlessShutdownPinRetains_
            <<" originalLowerCalls="<<app->headlessShutdownOriginalLowerCalls_<<" plainCloseCalls="<<app->headlessShutdownPlainCloseCalls_
            <<" goneCloseCalls="<<app->headlessShutdownGoneCloseCalls_<<" savedLower="<<app->headlessShutdownSavedLower_
            <<" actualReceiver="<<app->headlessShutdownActualReceiver_<<" ";
        printFacts("before",beforeProbe);std::cout<<" ";printFacts("after",afterProbe);std::cout<<"\n";std::cout.flush();
        if(probeFailure)std::rethrow_exception(probeFailure);
        require(probes==1&&app->headlessShutdownPinCaptureProbes_==1&&!app->headlessBeforeShutdownPinRetain_&&
            app->headlessShutdownPinRetains_==0&&app->headlessShutdownCaptureDenied_&&!app->headlessShutdownCaptureReady_&&
            resolved.empty()&&!app->headlessShutdownPinResultOverflow_,
            "Controlled retain reentry was not irrevocably denied before the actual source AddRef");
        require(beforeProbe.ownedCloseEntryTick&&beforeProbe.ownedCloseEntryTick<=probeTick&&probeTick<=probeReturnTick&&
            probeReturnTick<=app->headlessShutdownDispatchTick_&&afterProbe.epoch>originalEpoch&&
            !beforeNative.overflow&&!afterNative.overflow,
            "Controlled capture/admission/native callback timing was late or inconsistent");
        for(UINT index=0;index<app->headlessShutdownPinResultCount_;++index)
            require(FAILED(app->headlessShutdownPinResults_[index].result),
                "Controlled capture reentry accepted a native provider pin callback");
        require(app->headlessShutdownOriginalLowerCalls_==0&&
            app->headlessShutdownPlainCloseCalls_==(destroy?0u:1u)&&app->headlessShutdownGoneCloseCalls_==(destroy?1u:0u)&&
            (destroy?app->headlessShutdownActualReceiver_==0:
                app->headlessShutdownActualReceiver_==reinterpret_cast<std::uintptr_t>(ExplorerApp::windowProc)),
            "Controlled capture reentry forwarded a stale original native lower receiver");
        require(!app->window_&&!IsWindow(originalWindow)&&app->closing_&&app->shutdownStatus_==S_OK&&
            !app->ribbon_.valid()&&!app->browser_&&!app->view_&&!app->folderView_&&!app->shutdownPinTransaction_&&
            !app->frequentPlacesTask_,"Controlled capture reentry did not finish actual original App host cleanup");
        app->ribbon_.reset();checkOwnedPinDeadline();require(resolved.empty()&&!app->ribbon_.valid(),"Controlled capture denial replayed a native pin");
        app->headlessShutdownPinResolved_={};originalFolder.Reset();checkOwnedPinDeadline();
        originalView.Reset();checkOwnedPinDeadline();app.Reset();checkOwnedPinDeadline();
        for(unsigned index=0;index<3;++index){folders.verify(index);checkOwnedPinDeadline();}
        bool isolated=false,visible=true;
        succeeded(desktop.verifyIsolation(&isolated),"Verify isolation after controlled first-retain source reentry");checkOwnedPinDeadline();
        succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Read input desktop visibility after controlled source reentry");checkOwnedPinDeadline();
        require(isolated&&!visible,"Controlled source reentry exposed input desktop UI");checkOwnedPinDeadline();
        std::cout<<"Observed: "<<label<<" (controlled-before-first-source-AddRef; originalLowerCalls=0; providerInvoke=0)\n";
    }
    static void run(const PrivateDesktop& desktop,RibbonLayout layout,ULONGLONG sharedDeadline) {
        OwnedFolders folders;
        const auto run=[&](unsigned presses,bool both,bool optIn,Reentry reentry,const char* description) {
            runCase(desktop,layout,folders,presses,both,optIn,reentry,description,sharedDeadline);
            ownedPinDeadline=sharedDeadline;checkOwnedPinDeadline();
        };
        run(0,false,true,Reentry::None,"actual App unchanged native final array");
        run(1,false,true,Reentry::None,"actual App one native pin accepted at close");
        run(1,true,true,Reentry::None,"actual App two original Shell targets resolved at close");
        run(2,false,true,Reentry::None,"actual App two presses suppress unchanged final pin");
        run(1,true,false,Reentry::None,"actual App ordinary headless close denies profile pins");
        run(1,true,true,Reentry::Reset,"actual App nested reset revokes remaining native pins");
        run(1,true,true,Reentry::Initialize,"actual App initialize entry revokes remaining native pins");
        run(1,true,true,Reentry::ReplaceRibbon,"actual App successful replacement revokes original native pin batch");
        run(1,true,true,Reentry::Close,"actual App reentered WM_CLOSE revokes remaining native pins");
        run(1,true,true,Reentry::DestroyOwner,"actual App native owner destruction revokes remaining native pins");
        run(1,true,true,Reentry::ReplaceDisplayedIdentity,"actual App displayed identity replacement rejects old native index");
        runCaptureBoundaryCase(desktop,layout,folders,false,"controlled first-retain real reset revokes original capture",sharedDeadline);
        ownedPinDeadline=sharedDeadline;checkOwnedPinDeadline();
        runCaptureBoundaryCase(desktop,layout,folders,true,"controlled first-retain real DestroyWindow revokes original capture",sharedDeadline);
        ownedPinDeadline=sharedDeadline;checkOwnedPinDeadline();
        folders.finish();checkOwnedPinDeadline();
    }
};
}

int wmain(int argc,wchar_t** argv) {
    if(argc<2||argc>3||std::wcscmp(argv[1],L"--owned-read-only")!=0)return 2;
    auto layout=explorer::RibbonLayout::Authored;
    if(argc==3) {if(std::wcscmp(argv[2],L"--installed")!=0)return 2;layout=explorer::RibbonLayout::InstalledWindows10;}
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;if(FAILED(desktop.initialize()))return 3;
    explorer::NativeApartmentOwner nativeApartment;
    const auto initialized=nativeApartment.initializeOle();if(FAILED(initialized))return 4;
    const auto sharedDeadline=GetTickCount64()+105000;ownedPinDeadline=sharedDeadline;
    int result=0;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};
        require(InitCommonControlsEx(&controls)!=FALSE,"Initialize actual App private controls");
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=DefWindowProcW;
        cls.lpszClassName=L"WindowsExplorerOwnedShutdownPinReplacement";
        require(RegisterClassW(&cls)!=0,"Register owned replacement host class");
        explorer::ShutdownPinsNativeFixture::run(desktop,layout,sharedDeadline);checkOwnedPinDeadline();
        require(UnregisterClassW(cls.lpszClassName,cls.hInstance)!=FALSE,"Release owned replacement host class");
    }catch(const explorer::ShutdownPinsNativeFixture::InstalledUnavailable& error) {
        result=77;std::cout<<"UNAVAILABLE: "<<error.what()<<'\n';
    }catch(const std::exception& error){result=1;std::cerr<<"FAIL: actual App shutdown pin resolution: "<<error.what()<<'\n';}
    if(FAILED(explorer::drainStaWorkers(5000))) {std::cerr<<"FAIL: final native worker drain\n";TerminateProcess(GetCurrentProcess(),8);std::_Exit(8);}
    bool isolated=false,visible=true;
    if(FAILED(desktop.verifyIsolation(&isolated))||!isolated||FAILED(desktop.visibleWindowsOnInputDesktop(visible))||visible)result=1;
    nativeApartment.finishOrTerminate();
    // Final OLE/provider releases precede the final input-desktop admission.
    isolated=false;visible=true;
    if(FAILED(desktop.verifyIsolation(&isolated))||!isolated||FAILED(desktop.visibleWindowsOnInputDesktop(visible))||visible)result=1;
    if(GetTickCount64()>=sharedDeadline){result=1;std::cerr<<"FAIL: shared deadline exceeded after final native releases\n";}
    if(result==0)std::cout<<"PASS: 11/11 original actual App close cases + 2/2 controlled first-retain reset/destroy cases; providerInvoke=0; pin persistence unproved\n";
    return result;
}
