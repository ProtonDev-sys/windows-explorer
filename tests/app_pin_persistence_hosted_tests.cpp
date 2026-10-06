#include "explorer/app.hpp"
#include "explorer/worker_sta.hpp"
#include "explorer/native_apartment.hpp"
#include "app_pin_persistence_accessibility.hpp"
#include "app_pin_persistence_hosted_admission.hpp"
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <bcrypt.h>

#if !defined(EXPLORER_HOSTED_PIN_PERSISTENCE_FIXTURE)
#error This fixture requires the separate explicitly enabled Hosted-only target.
#endif

namespace explorer {
struct HostedPinPersistenceFixture {
    struct OwnedTarget {
        std::filesystem::path root,path;
        ComPtr<IShellItem> item;
        NativeHandle protectedDirectory;
        NativeHandle protectedRoot;
        FILE_ID_INFO original{},originalRoot{};
        bool createdRoot=false,createdTarget=false,finished=false;
        explicit OwnedTarget(const HostedAdmission& admission) {
            GUID guid{};succeeded(CoCreateGuid(&guid),"Create fresh owned Shell target GUID");
            wchar_t text[40]{};require(StringFromGUID2(guid,text,40)>0,"Format owned target GUID");
            root=admission.temporaryPath/(std::wstring(L"ExplorerHostedPinRoundTrip-")+text);
            path=root/(std::wstring(L"OwnedPin-")+text);
            require(root.is_absolute(),"Owned GUID root is not absolute");
            try {
                createdRoot=std::filesystem::create_directory(root);require(createdRoot,"Create exclusively new GUID root");
                protectedRoot.value=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                require(protectedRoot.value!=INVALID_HANDLE_VALUE,"Protect exact owned root against rebinding");originalRoot=nativeIdentity(protectedRoot.value,true);
                createdTarget=std::filesystem::create_directory(path);require(createdTarget,"Create exclusively new GUID folder");
                succeeded(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item)),"Create actual original owned Shell item");
                protectedDirectory.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                require(protectedDirectory.value!=INVALID_HANDLE_VALUE,"Protect owned original folder against write/delete rebinding");
                original=nativeIdentity(protectedDirectory.value,true);verify(item.Get());
            }catch(...) {release();std::error_code ignored;if(createdTarget)std::filesystem::remove(path,ignored);if(createdRoot)std::filesystem::remove(root,ignored);throw;}
        }
        ~OwnedTarget() {
            release();
            if(!finished)std::cerr<<"CleanupReceipt ownedTemporaryFolders=RETAINED profileRestorationNotYetVerified=1\n";
        }
        void release() noexcept {
            item.Reset();if(protectedDirectory.value!=INVALID_HANDLE_VALUE){CloseHandle(protectedDirectory.value);protectedDirectory.value=INVALID_HANDLE_VALUE;}
            if(protectedRoot.value!=INVALID_HANDLE_VALUE){CloseHandle(protectedRoot.value);protectedRoot.value=INVALID_HANDLE_VALUE;}
        }
        void verify(IShellItem* actual) const {
            require(actual&&item&&sameNativeIdentity(original,nativeIdentity(protectedDirectory.value,true)),"Retained original owned target identity changed");
            NativeHandle rootNow(CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            require(rootNow.value!=INVALID_HANDLE_VALUE&&sameNativeIdentity(originalRoot,nativeIdentity(rootNow.value,true))&&
                sameNativeIdentity(originalRoot,nativeIdentity(protectedRoot.value,true)),"Owned original root pathname was rebound");
            int order=1;succeeded(actual->Compare(item.Get(),SICHINT_CANONICAL,&order),"Compare actual owned target canonically");
            require(order==0,"Actual Shell item is not the original owned canonical target");
            const auto actualPath=filesystemName(actual);
            NativeHandle current(CreateFileW(actualPath.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            require(current.value!=INVALID_HANDLE_VALUE&&sameNativeIdentity(original,nativeIdentity(current.value,true)),
                "Actual Shell target does not retain the original full volume/128-bit FileID");
        }
        void finish() {
            verify(item.Get());release();
            checkOwnedPinDeadline();require(createdTarget&&createdRoot,"Owned folder cleanup receipts were already consumed");
            const auto targetRemoved=std::filesystem::remove(path);if(targetRemoved)createdTarget=false;
            require(targetRemoved,"Remove exact empty owned GUID folder after profile restoration");
            const auto rootRemoved=std::filesystem::remove(root);if(rootRemoved)createdRoot=false;
            finished=targetRemoved&&rootRemoved;require(rootRemoved,"Remove exact empty owned GUID root after profile restoration");
        }
    };
    struct NativePlace {
        ComPtr<IShellItem> item;
        std::vector<BYTE> fullPidl;
        bool pinned=false,filesystem=false,fileIdentityKnown=false;
        HRESULT fileIdentityStatus=E_PENDING;
        FILE_ID_INFO file{};
    };
    using Inventory=std::vector<NativePlace>;
    struct PlainPin {std::vector<BYTE> fullPidl;bool filesystem=false;FILE_ID_INFO file{};};
    struct Result {
        int outcome=1;bool persistenceProved=false,originalInventoryRead=false,ownedFoldersRemoved=false,appTeardownProved=false;
        ULONGLONG primaryDeadline=0,cleanupDeadline=0;std::vector<PlainPin> originalPins;
    };
    static std::string hex(const BYTE* bytes,size_t count) {
        std::ostringstream text;text<<std::hex<<std::setfill('0');
        for(size_t index=0;index<count;++index)text<<std::setw(2)<<static_cast<unsigned>(bytes[index]);return text.str();
    }
    static std::string pidlDigest(const std::vector<BYTE>& bytes) {
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<BYTE> storage;
        struct Crypto {BCRYPT_ALG_HANDLE& algorithm;BCRYPT_HASH_HANDLE& hash;
            ~Crypto(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}} crypto{algorithm,hash};
        require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"Open receipt SHA256");
        DWORD length=0,read=0;
        require(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&read,0)>=0&&read==sizeof(length),"Read receipt hash storage size");
        storage.resize(length);std::array<BYTE,32> digest{};
        require(BCryptCreateHash(algorithm,&hash,storage.data(),length,nullptr,0,0)>=0&&
            BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0&&
            BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,"Hash exact full native PIDL receipt");
        // Hash object must retire before its backing storage is destroyed.
        const auto destroyed=BCryptDestroyHash(hash);hash=nullptr;require(destroyed>=0,"Release receipt hash object");return hex(digest.data(),digest.size());
    }
    static void recordInventory(const char* stage,const Inventory& inventory,const OwnedTarget& target) {
        size_t pinned=0;
        for(size_t index=0;index<inventory.size();++index)if(inventory[index].pinned) {
            const auto& row=inventory[index];++pinned;
            std::cout<<"PinnedInventoryEntry stage="<<stage<<" nativeIndex="<<index<<" fullPIDLBytes="<<row.fullPidl.size()
                <<" fullPIDLSHA256="<<pidlDigest(row.fullPidl)<<" filesystem="<<row.filesystem;
            if(row.filesystem)std::cout<<" volumeSerial="<<row.file.VolumeSerialNumber<<" FileID128="<<hex(row.file.FileId.Identifier,16);
            std::cout<<" owned="<<ownedPlace(row,target)<<'\n';
        }
        std::cout<<"CompletePinnedInventoryReceipt stage="<<stage<<" enumerationExhausted=1 nativeRows="<<inventory.size()<<" pinned="<<pinned
            <<" ownedVolumeSerial="<<target.original.VolumeSerialNumber<<" ownedFileID128="<<hex(target.original.FileId.Identifier,16)<<'\n';
    }
    static Inventory inventory() {
        ComPtr<IShellItem> home;ComPtr<IShellFolder> folder;ComPtr<IEnumIDList> enumeration;
        succeeded(SHCreateItemFromParsingName(L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",nullptr,IID_PPV_ARGS(&home)),"Read actual native Home source");
        succeeded(home->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder)),"Bind original complete native Home folder");
        const auto opened=folder->EnumObjects(nullptr,SHCONTF_FOLDERS,&enumeration);
        require(opened==S_OK||opened==S_FALSE,"Complete native Home enumeration failed");
        if(opened==S_FALSE){require(!enumeration,"Empty native Home returned an ambiguous enumerator");return {};}
        require(enumeration!=nullptr,"Native Home complete enumerator is absent");Inventory result;
        // Safety bound is rejection, never a successful truncated inventory.
        bool exhausted=false;
        while(!exhausted) {
            checkOwnedPinDeadline();PIDLIST_RELATIVE raw=nullptr;ULONG fetched=0;
            const auto next=enumeration->Next(1,&raw,&fetched);Pidl child(raw);
            if(next==S_FALSE){require(fetched==0&&!child,"Native Home end-of-enumeration is ambiguous");exhausted=true;break;}
            require(next==S_OK&&fetched==1&&child,"Complete native Home iteration failed");
            require(result.size()<1024,"Complete native Home inventory exceeded safety bound; inventory is unproved");
            NativePlace place;ComPtr<IShellItem2> extended;ComPtr<IPropertyStore> properties;
            succeeded(SHCreateItemWithParent(nullptr,folder.Get(),child.get(),IID_PPV_ARGS(&place.item)),"Resolve exact native Home child");
            succeeded(place.item.As(&extended),"Read actual Home child property identity");
            succeeded(extended->GetPropertyStore(GPS_FASTPROPERTIESONLY|GPS_BESTEFFORT,IID_PPV_ARGS(&properties)),"Read actual native Home pin property store");
            PROPVARIANT pin{};struct Pin {PROPVARIANT& value;~Pin(){PropVariantClear(&value);}} pinGuard{pin};BOOL value=FALSE;
            succeeded(properties->GetValue(PKEY_Home_IsPinned,&pin),"Read original actual native IsPinned property");
            succeeded(PropVariantToBoolean(pin,&value),"Decode actual native IsPinned without assuming missing means false");place.pinned=value!=FALSE;
            PIDLIST_ABSOLUTE absolute=nullptr;const auto identityRead=SHGetIDListFromObject(place.item.Get(),&absolute);Pidl owned(absolute);
            succeeded(identityRead,"Read complete native Home child PIDL");require(owned!=nullptr,"Full native Home PIDL result is null");
            const auto bytes=ILGetSize(owned.get());require(bytes>=sizeof(USHORT)&&bytes<=65536,"Full native Home child PIDL is invalid");
            place.fullPidl.assign(reinterpret_cast<const BYTE*>(owned.get()),reinterpret_cast<const BYTE*>(owned.get())+bytes);
            SFGAOF attributes=SFGAO_FILESYSTEM;succeeded(place.item->GetAttributes(attributes,&attributes),"Read native Home filesystem identity classification");
            place.filesystem=(attributes&SFGAO_FILESYSTEM)!=0;
            if(place.filesystem) {
                const auto path=filesystemName(place.item.Get());NativeHandle handle(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                if(handle.value!=INVALID_HANDLE_VALUE) {
                    FILE_ATTRIBUTE_TAG_INFO tag{};
                    place.fileIdentityKnown=GetFileInformationByHandleEx(handle.value,FileIdInfo,&place.file,sizeof(place.file))!=FALSE&&
                        GetFileInformationByHandleEx(handle.value,FileAttributeTagInfo,&tag,sizeof(tag))!=FALSE&&
                        (tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
                    place.fileIdentityStatus=place.fileIdentityKnown?S_OK:E_ACCESSDENIED;
                }else place.fileIdentityStatus=HRESULT_FROM_WIN32(GetLastError());
                require(!place.pinned||place.fileIdentityKnown,"Complete pinned inventory filesystem identity is inaccessible/reparse/unproved");
            }
            for(const auto& previous:result)require(!samePlace(previous,place),"Complete native Home inventory contains ambiguous duplicate canonical identities");
            result.push_back(std::move(place));
        }
        require(exhausted,"Native pinned inventory is incomplete");return result;
    }
    static bool samePlace(const NativePlace& left,const NativePlace& right) {
        int order=1;const auto compared=left.item->Compare(right.item.Get(),SICHINT_CANONICAL,&order);
        succeeded(compared,"Compare complete original native pinned identity");
        return order==0&&left.filesystem==right.filesystem&&(!left.filesystem||
            (left.fileIdentityKnown&&right.fileIdentityKnown&&sameNativeIdentity(left.file,right.file)));
    }
    static bool ownedPlace(const NativePlace& place,const OwnedTarget& target) {
        int order=1;succeeded(place.item->Compare(target.item.Get(),SICHINT_CANONICAL,&order),"Compare native Home row to original owned target");
        if(order!=0)return false;target.verify(place.item.Get());
        require(place.filesystem&&place.fileIdentityKnown&&sameNativeIdentity(place.file,target.original),"Owned Home row lost full native FileID");return true;
    }
    static bool targetPinned(const Inventory& current,const OwnedTarget& target) {
        unsigned matches=0;bool pinned=false;
        for(const auto& place:current)if(ownedPlace(place,target)){++matches;pinned=place.pinned;}
        require(matches<=1,"Owned Home canonical target is duplicated");return pinned;
    }
    static void originalUnchanged(const Inventory& original,const Inventory& current,const OwnedTarget& target) {
        size_t expected=0,observed=0;
        for(const auto& place:original)if(place.pinned) {
            require(!ownedPlace(place,target),"Fresh GUID target was already in the original pinned profile inventory");++expected;
            size_t matches=0;for(const auto& actual:current)if(actual.pinned&&samePlace(place,actual))++matches;
            require(matches==1,"Original complete pinned profile inventory changed; fixture cannot repair unowned profile state");
        }
        for(const auto& place:current)if(place.pinned&&!ownedPlace(place,target))++observed;
        require(expected==observed,"Unowned pinned profile membership changed; fixture will not reset the profile");
    }
    static void exactOriginalPins(const Inventory& original,const Inventory& current) {
        size_t expected=0,observed=0;
        for(const auto& row:original)if(row.pinned) {
            ++expected;size_t matches=0;for(const auto& actual:current)if(actual.pinned&&samePlace(row,actual))++matches;
            require(matches==1,"Original complete pinned inventory changed after owned temporary cleanup/releases");
        }
        for(const auto& row:current)if(row.pinned)++observed;
        require(expected==observed,"Additional pinned profile membership appeared after owned temporary cleanup/releases");
    }
    static void recordCallbacks(const ExplorerApp& app,const char* stage) {
        std::cerr<<"ActualCallbackReceiptSummary stage="<<stage<<" nativeInvokes="<<app.hostedPinInvokeCount_<<" finalCallbackReturns="<<app.hostedPinReturnCount_
            <<" normalCallbackReturns="<<app.hostedNormalPinReturnCount_<<" overflow="<<app.hostedPinReceiptOverflow_<<" originalNormalPostQuit="<<app.hostedNormalPostQuitCount_<<'\n';
        for(UINT index=0;index<app.hostedNormalPinReturnCount_;++index) {
            const auto& value=app.hostedNormalPinReturns_[index];std::cerr<<"ActualNormalCallbackReceipt ordinal="<<index<<" rawHRESULT="
                <<static_cast<unsigned long>(value.result)<<" originalIndex="<<value.index<<" requested="<<value.requested<<" closingEntry/After="
                <<value.closingEntry<<'/'<<value.closingAfter<<" generation="<<value.generation<<" revision="<<value.revision<<'\n';
        }
        for(UINT index=0;index<app.hostedPinInvokeCount_;++index) {
            const auto& value=app.hostedPinInvokes_[index];std::cerr<<"ActualInvokeReceipt ordinal="<<index<<" rawHRESULT="
                <<static_cast<unsigned long>(value.result)<<" requested="<<value.requested<<" menuId="<<value.menuId
                <<" originalArray="<<reinterpret_cast<uintptr_t>(value.array)<<" originalItem="<<reinterpret_cast<uintptr_t>(value.item)<<'\n';
        }
        for(UINT index=0;index<app.hostedPinReturnCount_;++index) {
            const auto& value=app.hostedPinReturns_[index];std::cerr<<"ActualFinalCallbackReceipt ordinal="<<index<<" rawHRESULT="
                <<static_cast<unsigned long>(value.result)<<" originalIndex="<<value.index<<" requested="<<value.requested<<" sourceCurrentAfter="
                <<value.currentAfter<<" generation="<<value.generation<<" revision="<<value.revision<<" actualCloseEntry="<<value.closeEntry<<'\n';
        }
    }
    struct Session {
        ComPtr<ExplorerApp> app;
        HWND originalWindow=nullptr;
        bool closeDispatched=false,quitConsumed=false;
        ~Session(){if(app&&app->window_)std::cerr<<"CleanupReceipt normalAppStillOpen=1\n";}
    };
    static void pump(Session* closing=nullptr) {
        MSG message{};
        for(unsigned count=0;count<256&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++count) {
            checkOwnedPinDeadline();
            if(message.message==WM_QUIT) {
                require(closing&&closing->closeDispatched&&!closing->quitConsumed&&message.wParam==0&&
                    closing->app&&!closing->app->headless_&&closing->app->closing_&&!closing->app->window_&&
                    !IsWindow(closing->originalWindow)&&SUCCEEDED(closing->app->shutdownStatus_)&&
                    closing->app->hostedNormalPostQuitCount_==1&&closing->app->hostedNormalPostQuitOwner_==closing->originalWindow&&
                    closing->app->hostedNormalPostQuitThread_==GetCurrentThreadId(),
                    "Unexpected process quit; only the owned normal App close may be consumed");
                closing->quitConsumed=true;std::cout<<"OwnedNormalPostQuitReceipt actualWM_QUIT=1 code=0 creatorTID="<<GetCurrentThreadId()<<'\n';continue;
            }
            TranslateMessage(&message);DispatchMessageW(&message);checkOwnedPinDeadline();
        }
        MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);checkOwnedPinDeadline();
    }
    static void close(Session& session) {
        if(session.app&&session.app->window_) {
            require(!session.closeDispatched,"Owned normal App close was already dispatched");session.closeDispatched=true;
            require(PostMessageW(session.originalWindow,WM_CLOSE,0,0)!=FALSE,"Post actual normal App WM_CLOSE");
        }
        while(session.app&&(session.app->window_||!session.quitConsumed))pump(&session);
        require(session.closeDispatched&&session.quitConsumed&&session.app&&session.app->closing_&&
            SUCCEEDED(session.app->shutdownStatus_)&&!session.app->ribbon_.valid()&&!session.app->shutdownPinTransaction_&&
            !session.app->browser_&&!session.app->view_&&!session.app->folderView_,
            "Actual normal App WM_CLOSE/final callback/WM_DESTROY teardown is incomplete");
    }
    static void create(Session& session,const HostedAdmission& admission,const PrivateDesktop& desktop,
        const OwnedTarget& target,RibbonLayout layout) {
        admission.normalAppAllowed(desktop);target.verify(target.item.Get());
        require(!session.app,"Fresh normal App creation must not overwrite an existing App session");
        session.originalWindow=nullptr;session.closeDispatched=false;session.quitConsumed=false;
        session.app.Attach(new ExplorerApp(GetModuleHandleW(nullptr),false,layout));
        target.verify(target.item.Get());
        session.app->hostedPinOwnedTarget_=target.item;session.app->hostedPinOwnedFile_=target.original;
        target.verify(target.item.Get());session.app->hostedPinReceiptsEnabled_=true;
        const auto created=session.app->create(target.root.native());session.originalWindow=session.app->window_;
        succeeded(created,"Create genuine normal production App on admitted fresh Hosted profile");DWORD process=0;
        require(session.originalWindow&&GetWindowThreadProcessId(session.originalWindow,&process)==GetCurrentThreadId()&&process==GetCurrentProcessId(),
            "Actual normal App escaped original private owner/thread");
        while(!session.app->view_||!session.app->folderView_||session.app->navigating_||session.app->commandRefreshActive_||session.app->commandStatesPending())pump();
        require(!session.app->headless_&&!session.app->closing_&&session.app->ribbon_.layout()==layout,
            "Actual normal App layout/headless/closing admission differs");
        if(layout==RibbonLayout::InstalledWindows10)succeeded(session.app->ribbon_.installedLayoutStatus(),"Require actual installed native Ribbon");
        // No row assignment: use the same native asynchronous Home read and UI
        // invalidation used by production. Wait for its actual publication.
        session.app->refreshFrequentPlaces();
        while(session.app->frequentPlacesTask_)pump();
        require(session.app->frequentPlacesReadAt_!=0,"Normal App native Home source never published");
        session.app->ribbon_.invalidate(RibbonFrequentPlaces);succeeded(session.app->ribbon_.flush(),"Publish genuine native App RecentItems source");
        admission.normalAppAllowed(desktop);target.verify(target.item.Get());
    }
    struct Source {
        struct Row {IShellItem* item=nullptr;std::wstring label,description;bool pinned=false;};
        UINT index=UI_COLLECTION_INVALIDINDEX;
        bool pinned=false;
        std::wstring label,description;
        HWND window=nullptr;DWORD thread=0;
        ComPtr<IShellItem> item;ComPtr<IShellView> site;ComPtr<IFolderView2> folder;ComPtr<IExplorerBrowser> browser;
        IShellView* originalSite=nullptr;IFolderView2* originalFolder=nullptr;IExplorerBrowser* originalBrowser=nullptr;
        Pidl location;UINT64 generation=0;unsigned navigation=0;std::uint64_t revision=0;
        std::vector<Row> rows;
    };
    static bool current(const ExplorerApp& app,const Source& source) {
        if(app.closing_||app.headless_||app.navigating_||app.window_!=source.window||!IsWindow(source.window)||GetCurrentThreadId()!=source.thread||
            app.view_.Get()!=source.originalSite||app.folderView_.Get()!=source.originalFolder||app.browser_.Get()!=source.originalBrowser||
            app.namespaceGeneration_!=source.generation||app.navigationCount_!=source.navigation||app.displayedFrequentPlacesRevision_!=source.revision||
            !app.currentPidl_||!source.location||app.displayedFrequentPlaces_.size()!=source.rows.size())return false;
        const auto bytes=ILGetSize(source.location.get());
        if(!bytes||bytes!=ILGetSize(app.currentPidl_.get())||std::memcmp(source.location.get(),app.currentPidl_.get(),bytes)!=0)return false;
        for(size_t index=0;index<source.rows.size();++index) {
            const auto& before=source.rows[index];const auto& actual=app.displayedFrequentPlaces_[index];
            if(before.item!=actual.item.Get()||before.label!=actual.label||before.description!=actual.description||before.pinned!=actual.pinned)return false;
        }
        return true;
    }
    static Source snapshot(ExplorerApp& app) {
        Source source;source.window=app.window_;source.thread=GetCurrentThreadId();
        source.originalSite=app.view_.Get();source.originalFolder=app.folderView_.Get();source.originalBrowser=app.browser_.Get();
        source.location.reset(ILCloneFull(app.currentPidl_.get()));source.generation=app.namespaceGeneration_;source.navigation=app.navigationCount_;
        source.revision=app.displayedFrequentPlacesRevision_;
        // Copy plain descriptors before any COM AddRef. A live vector reference
        // must never cross pumping retention, Compare, or interface Release.
        for(const auto& row:app.displayedFrequentPlaces_)source.rows.push_back({row.item.Get(),row.label,row.description,row.pinned});
        require(current(app,source),"Original normal App source changed before retention");
        source.site=source.originalSite;require(current(app,source),"Original site AddRef changed normal App source");
        source.folder=source.originalFolder;require(current(app,source),"Original folder AddRef changed normal App source");
        source.browser=source.originalBrowser;require(current(app,source),"Original browser AddRef changed normal App source");return source;
    }
    static Source capture(ExplorerApp& app,const OwnedTarget& target,bool pinned) {
        auto source=snapshot(app);
        for(size_t index=0;index<source.rows.size();++index) {
            require(current(app,source),"Original source changed before row retention");
            ComPtr<IShellItem> retained=source.rows[index].item;require(current(app,source),"Native row AddRef changed source");
            int order=1;succeeded(retained->Compare(target.item.Get(),SICHINT_CANONICAL,&order),"Locate original owned canonical row in genuine normal App source");
            require(current(app,source),"Native canonical row comparison changed source");
            if(order!=0)continue;require(source.index==UI_COLLECTION_INVALIDINDEX,"Normal App native source duplicates owned canonical target");
            source.index=static_cast<UINT>(index);source.item=retained;source.pinned=source.rows[index].pinned;
            source.label=source.rows[index].label;source.description=source.rows[index].description;target.verify(source.item.Get());
        }
        require(source.index!=UI_COLLECTION_INVALIDINDEX,"Owned target was not admitted by genuine native App Home source; no fabricated row is permitted");
        unsigned labels=0;for(const auto& row:source.rows)if(row.label==source.label)++labels;
        require(labels==1,"Owned target native label is ambiguous for public accessibility");
        require(!source.label.empty()&&source.pinned==pinned&&current(app,source),"Actual owned normal App pin source is stale or has the wrong initial state");return source;
    }
    static void resolvedNativeLeaf(ExplorerApp& app,const Source& source,const OwnedTarget& target,bool requested) {
        require(current(app,source),"Actual original App source changed before native leaf evidence");
        ComPtr<IShellItemArray> array;ComPtr<IShellItem> actual;NativeContextMenu menu;std::vector<ContextMenuEntry> leaves;
        succeeded(SHCreateShellItemArrayFromShellItem(source.item.Get(),IID_PPV_ARGS(&array)),"Create actual native single original source array");
        DWORD count=0;succeeded(array->GetCount(&count),"Read actual resolved native array count");require(count==1,"Actual resolved native pin array is not single-owned-target");
        succeeded(array->GetItemAt(0,&actual),"Read original actual native array identity");target.verify(actual.Get());
        require(current(app,source),"Native array identity read pumped a stale App source");
        succeeded(menu.createSelection(source.window,array.Get(),source.site.Get()),"Bind actual native provider to original normal App item/view site");
        succeeded(menu.enumerate(leaves,false),"Resolve real native pin/unpin leaf without invoking it");
        const auto wanted=requested?L"pintohome":L"unpinfromhome";
        const auto found=std::find_if(leaves.begin(),leaves.end(),[&](const auto& leaf){return leaf.canonicalVerb==wanted&&leaf.enabled()&&!leaf.submenu;});
        require(found!=leaves.end(),"Actual enabled owned pin/unpin native leaf is absent");
        std::cout<<"ActualNativeLeafReceipt canonical="<<(requested?"pintohome":"unpinfromhome")<<" arrayCount="<<count<<" menuId="<<found->id<<" providerInvoke=0\n";
        menu.reset();actual.Reset();array.Reset();
        require(current(app,source),"Native leaf or its temporary releases pumped a stale original App source");target.verify(source.item.Get());
    }
    static void nativeArray(ExplorerApp& app,const Source& source) {
        require(current(app,source),"Original source is stale before native RecentItems read");
        PROPVARIANT value{};struct Clear {PROPVARIANT& value;~Clear(){PropVariantClear(&value);}} clear{value};
        succeeded(app.ribbon_.framework()->GetUICommandProperty(RibbonFrequentPlaces,UI_PKEY_RecentItems,&value),"Read actual native RecentItems array from normal App");
        require(value.vt==(VT_ARRAY|VT_UNKNOWN)&&value.parray&&SafeArrayGetDim(value.parray)==1,"Actual native RecentItems shape is unavailable");
        LONG first=0,last=-1;succeeded(SafeArrayGetLBound(value.parray,1,&first),"Read actual native RecentItems lower bound");
        succeeded(SafeArrayGetUBound(value.parray,1,&last),"Read actual native RecentItems upper bound");
        require(last>=first&&last-first+1<=64&&static_cast<size_t>(last-first+1)>=source.rows.size(),"Actual native RecentItems omits original source rows");
        for(size_t index=0;index<source.rows.size();++index) {
            LONG position=first+static_cast<LONG>(index);IUnknown* raw=nullptr;ComPtr<IUnknown> unknown;ComPtr<IUISimplePropertySet> properties;
            const auto elementRead=SafeArrayGetElement(value.parray,&position,&raw);unknown.Attach(raw);
            succeeded(elementRead,"Read actual native RecentItems original row");
            require(unknown!=nullptr,"Actual native RecentItems original row is null");succeeded(unknown.As(&properties),"Read actual native RecentItems row properties");
            PROPVARIANT label{},description{},pin{};
            struct Values {PROPVARIANT& a;PROPVARIANT& b;PROPVARIANT& c;~Values(){PropVariantClear(&a);PropVariantClear(&b);PropVariantClear(&c);}} values{label,description,pin};
            succeeded(properties->GetValue(UI_PKEY_Label,&label),"Read actual native row label");
            succeeded(properties->GetValue(UI_PKEY_LabelDescription,&description),"Read actual native row original description");
            succeeded(properties->GetValue(UI_PKEY_Pinned,&pin),"Read actual native row original pin state");BOOL pinned=FALSE;
            succeeded(PropVariantToBoolean(pin,&pinned),"Decode actual native row original pin state");
            require(label.vt==VT_LPWSTR&&label.pwszVal&&source.rows[index].label==label.pwszVal&&
                description.vt==VT_LPWSTR&&description.pwszVal&&source.rows[index].description==description.pwszVal&&
                source.rows[index].pinned==(pinned!=FALSE),"Actual native RecentItems original mapping differs from captured actual App source");
            require(current(app,source),"Native row property read pumped a stale original source");
        }
        PropVariantClear(&value);value={};require(current(app,source),"Actual native RecentItems temporary releases pumped stale source");
    }
    static void mutate(Session& session,const HostedAdmission& admission,const PrivateDesktop& desktop,
        const OwnedTarget& target,bool requested,bool finalOnly,bool cleanupOnly=false) {
        auto beforeOpen=capture(*session.app.Get(),target,!requested);admission.normalAppAllowed(desktop);
        const auto returnsBefore=session.app->hostedPinReturnCount_,invokesBefore=session.app->hostedPinInvokeCount_;
        const auto normalBefore=session.app->hostedNormalPinReturnCount_;
        require(SetWindowPos(beforeOpen.window,nullptr,0,0,0,0,SWP_SHOWWINDOW|SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER)!=FALSE,
            "Realize original normal App private HWND without activation");
        require(IsWindowVisible(beforeOpen.window)&&current(*session.app.Get(),beforeOpen),"Native accessibility realization replaced original source");
        auto handshake=std::make_shared<ActionHandshake>();
        auto action=std::async(std::launch::async,openAndPress,GetThreadDesktop(beforeOpen.thread),beforeOpen.thread,beforeOpen.pinned,1,
            beforeOpen.label,beforeOpen.window,ownedPinDeadline,handshake);
        struct CancelAction {std::shared_ptr<ActionHandshake> state;~CancelAction(){state->cancelled=true;}} cancelAction{handshake};
        while(!handshake->ready&&action.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump();
        require(handshake->ready,"Actual owned native File menu/row was not available before pin admission");
        // Opening the real menu may normally request a new source revision.
        // Capture its actual displayed source while the worker waits, then
        // fence every native evidence read before admitting the sole UI press.
        auto source=capture(*session.app.Get(),target,!requested);
        require(source.window==beforeOpen.window&&source.originalSite==beforeOpen.originalSite&&source.originalFolder==beforeOpen.originalFolder&&
            source.generation==beforeOpen.generation&&source.navigation==beforeOpen.navigation&&source.label==beforeOpen.label,
            "Native menu opening replaced the owned original App view/target");
        resolvedNativeLeaf(*session.app.Get(),source,target,requested);nativeArray(*session.app.Get(),source);
        admission.normalAppAllowed(desktop);require(current(*session.app.Get(),source),"Normal App source changed before admitting native press");handshake->admitted=true;
        while(action.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump();
        const auto actual=action.get();succeeded(actual.result,"Operate actual normal App owned native pin control");
        require(actual.opened&&actual.presses==1&&actual.finalPressed==requested,
            "Actual owned native pin control state/source differs");
        target.verify(source.item.Get());
        const auto normalBeforeClose=session.app->hostedNormalPinReturnCount_,invokesBeforeClose=session.app->hostedPinInvokeCount_;
        const auto finalBeforeClose=session.app->hostedPinReturnCount_;
        const bool exactBeforeClose=current(*session.app.Get(),source);
        // WM_CLOSE follows the actual still-open native menu. There is no fake
        // Execute, fabricated array, direct pin callback, or direct menu Invoke.
        close(session);
        std::cout<<"ActualDeliveryReceipt normalBeforeClose="<<normalBeforeClose-normalBefore<<" invokesBeforeClose="<<invokesBeforeClose-invokesBefore
            <<" finalBeforeClose="<<finalBeforeClose-returnsBefore<<" finalAfterClose="<<session.app->hostedPinReturnCount_-returnsBefore
            <<" requested="<<requested<<" selectedCase="<<(finalOnly?"final-destroy-only":"normal-flow")<<" cleanup="<<cleanupOnly<<'\n';
        if(cleanupOnly) {
            require(!requested&&!session.app->hostedPinReceiptOverflow_,"Cleanup may only unpin owned target and must retain actual receipts");
            for(UINT index=invokesBefore;index<session.app->hostedPinInvokeCount_;++index) {
                auto& invoked=session.app->hostedPinInvokes_[index];
                require(invoked.item==source.item.Get()&&invoked.site==source.site.Get()&&invoked.owner==source.window&&
                    !invoked.requested&&std::wstring(invoked.verb)==L"unpinfromhome"&&invoked.retainedArray.Get()==invoked.array,
                    "Actual cleanup production invocation reached an unowned or wrong native target");
                DWORD count=0;ComPtr<IShellItem> bound;
                succeeded(invoked.retainedArray->GetCount(&count),"Read actual cleanup provider array count");
                require(count==1,"Actual cleanup provider array contains an unowned extra target");
                succeeded(invoked.retainedArray->GetItemAt(0,&bound),"Read actual cleanup provider array original target");target.verify(bound.Get());
                std::cout<<"CleanupInvokeReceipt actualHRESULT="<<static_cast<unsigned long>(invoked.result)<<" requested=0 ownedTargetVerified=1 finalCallbackCount="
                    <<session.app->hostedPinReturnCount_-returnsBefore<<" finalPersistenceTestAcceptance=0\n";
                bound.Reset();invoked.retainedArray.Reset();
            }
            require(session.app->closing_&&!session.app->window_&&!session.app->ribbon_.valid(),"Actual cleanup native array releases changed owned close teardown");
            target.verify(source.item.Get());admission.normalAppAllowed(desktop);return;
        }
        if(!finalOnly) {
            require(!session.app->hostedPinReceiptOverflow_&&normalBeforeClose>normalBefore&&session.app->hostedPinInvokeCount_>invokesBefore,
                "Genuine normal pin-flow callback/provider invocation was not observed; final timing cannot substitute normal evidence");
            for(UINT index=normalBefore;index<session.app->hostedNormalPinReturnCount_;++index) {
                const auto& returned=session.app->hostedNormalPinReturns_[index];
                require(returned.result==S_OK&&!returned.closingEntry&&returned.item&&returned.requested==requested&&
                    returned.owner==source.window&&returned.site==source.site.Get(),
                    "Actual normal App callback failed or reached a different original owned source");
                const bool actualBinding=std::any_of(session.app->hostedPinInvokes_.begin()+invokesBefore,
                    session.app->hostedPinInvokes_.begin()+session.app->hostedPinInvokeCount_,[&](const auto& invoked){
                        return invoked.item==returned.item&&invoked.site==returned.site&&invoked.owner==returned.owner&&invoked.requested==requested;});
                require(actualBinding,"Actual normal callback has no corresponding real provider binding; fresh-source reload cannot substitute it");
            }
            for(UINT index=invokesBefore;index<session.app->hostedPinInvokeCount_;++index) {
                auto& invoked=session.app->hostedPinInvokes_[index];
                require(invoked.result==S_OK&&invoked.item&&invoked.site==source.site.Get()&&invoked.owner==source.window&&
                    invoked.requested==requested&&std::wstring(invoked.verb)==(requested?L"pintohome":L"unpinfromhome")&&
                    invoked.retainedArray.Get()==invoked.array,"Actual normal-flow provider invocation reached a wrong/failing/unowned leaf");
                DWORD count=0;ComPtr<IShellItem> bound;succeeded(invoked.retainedArray->GetCount(&count),"Read actual normal-flow provider array count");
                require(count==1,"Actual normal-flow provider array contains an unowned extra target");
                succeeded(invoked.retainedArray->GetItemAt(0,&bound),"Read exact actual normal-flow provider bound item");target.verify(bound.Get());
                bound.Reset();invoked.retainedArray.Reset();
            }
            recordCallbacks(*session.app.Get(),"normal-flow-after-close"); // Final returns grant no normal-flow acceptance.
            require(session.app->closing_&&!session.app->window_&&!session.app->ribbon_.valid(),"Actual normal-flow array Release changed completed normal App teardown");
            target.verify(source.item.Get());admission.normalAppAllowed(desktop);
            std::cout<<"NormalAppPinReceipt actualNormalCallbackAndInvokeS_OK=1 requested="<<requested<<" finalDestroyAcceptance=0\n";return;
        }
        require(exactBeforeClose&&normalBeforeClose==normalBefore&&invokesBeforeClose==invokesBefore&&finalBeforeClose==returnsBefore,
            "FINAL_DESTROY_TIMING_NOT_OBSERVED: actual native batch arrived through normal flow before close or source changed");
        require(!session.app->hostedPinReceiptOverflow_&&session.app->hostedPinReturnCount_==returnsBefore+1&&
            session.app->hostedPinInvokeCount_==invokesBefore+1,"Actual normal App final callback/provider Invoke receipts are missing or duplicated");
        const auto& returned=session.app->hostedPinReturns_[returnsBefore];const auto& invoked=session.app->hostedPinInvokes_[invokesBefore];
        require(returned.index==source.index&&returned.before==source.pinned&&returned.requested==requested&&returned.result==S_OK&&returned.currentAfter&&
            returned.owner==source.window&&returned.item==source.item.Get()&&returned.site==source.site.Get()&&
            returned.generation==source.generation&&returned.navigation==source.navigation&&returned.revision==source.revision&&returned.closeEntry!=0,
            "Actual complete normal App final callback returned failure, stale authority, or a different original target");
        require(invoked.array&&invoked.item==source.item.Get()&&invoked.site==source.site.Get()&&invoked.owner==source.window&&
            invoked.menuId!=0&&invoked.requested==requested&&invoked.result==S_OK&&
            std::wstring(invoked.verb)==(requested?L"pintohome":L"unpinfromhome"),
            "Actual normal production provider invocation differs from the owned enabled leaf");
        require(invoked.retainedArray.Get()==invoked.array,"Actual provider's original array retention receipt is absent");
        DWORD boundCount=0;ComPtr<IShellItem> bound;
        succeeded(invoked.retainedArray->GetCount(&boundCount),"Read exact actual production provider array count after complete callback");
        require(boundCount==1,"Actual production provider array includes an unowned additional target");
        succeeded(invoked.retainedArray->GetItemAt(0,&bound),"Read exact actual production provider bound target after complete callback");
        target.verify(bound.Get());bound.Reset();session.app->hostedPinInvokes_[invokesBefore].retainedArray.Reset();
        require(session.app->closing_&&!session.app->window_&&!IsWindow(source.window)&&!session.app->ribbon_.valid()&&
            session.app->hostedPinReturnCount_==returnsBefore+1&&session.app->hostedPinInvokeCount_==invokesBefore+1,
            "Actual provider array read/release changed completed owned normal App teardown");
        target.verify(source.item.Get());admission.normalAppAllowed(desktop);
        std::cout<<"FinalAppPinReceipt normalApp=1 actualWM_CLOSE=1 requested="<<requested<<" actualCallbackHRESULT=0 actualProviderInvokeHRESULT=0 originalIndex="
            <<source.index<<" creatorTID="<<source.thread<<" generation="<<source.generation<<" revision="<<source.revision<<" staleSuccess=0\n";
    }
    static Result run(const HostedAdmission& admission,const PrivateDesktop& desktop,RibbonLayout layout,bool finalOnly) {
        OwnedTarget target(admission);Inventory original;
        bool baselineRead=false,restored=false,persistenceProved=false;
        int outcome=0;std::string failure;
        Session session;
        const auto mainDeadline=GetTickCount64()+180000;ownedPinDeadline=mainDeadline;
        Result result;result.primaryDeadline=mainDeadline;
        try {
            original=inventory();baselineRead=true;require(!targetPinned(original,target),"Fresh owned GUID target already pinned");
            recordInventory("original",original,target);
            std::cout<<"OriginalCompletePinnedInventoryReceipt exhausted=1 nativeRows="<<original.size()<<" ownedTargetPinned=0\n";
            // Environmental recent-membership setup only; never a pin Invoke.
            // Windows may still omit it. Actual Home/App admission is mandatory.
            admission.normalAppAllowed(desktop);SHAddToRecentDocs(SHARD_SHELLITEM,target.item.Get());
            auto seeded=inventory();originalUnchanged(original,seeded,target);require(!targetPinned(seeded,target),"Recent registration unexpectedly changed owned pin state");
            std::cout<<"OwnedRecentAdmissionReceipt SHAddToRecentDocsAttempted=1 nativeMembershipStillRequired=1 pinnedInventoryUnchanged=1\n";
            create(session,admission,desktop,target,layout);mutate(session,admission,desktop,target,true,finalOnly);session.app.Reset();
            auto pinned=inventory();originalUnchanged(original,pinned,target);require(targetPinned(pinned,target),"Actual normal App pin callback did not persist native pinned state");
            recordInventory("after-actual-final-pin",pinned,target);
            create(session,admission,desktop,target,layout);auto pinnedSource=capture(*session.app.Get(),target,true);
            resolvedNativeLeaf(*session.app.Get(),pinnedSource,target,false);nativeArray(*session.app.Get(),pinnedSource);
            mutate(session,admission,desktop,target,false,finalOnly);session.app.Reset();
            auto unpinned=inventory();originalUnchanged(original,unpinned,target);require(!targetPinned(unpinned,target),"Actual normal App unpin callback did not persist native unpinned state");
            recordInventory("after-actual-final-unpin",unpinned,target);
            // A third fresh normal App reads the real source. Missing unpinned
            // owned row is allowed only if the complete native Home source also
            // omits it; no retained earlier App cache grants this observation.
            create(session,admission,desktop,target,layout);
            auto freshSource=snapshot(*session.app.Get());unsigned admitted=0;
            for(const auto& row:freshSource.rows) {
                require(current(*session.app.Get(),freshSource),"Fresh normal App source changed before persistence row retention");
                ComPtr<IShellItem> retained=row.item;require(current(*session.app.Get(),freshSource),"Fresh native row AddRef changed source");
                int order=1;succeeded(retained->Compare(target.item.Get(),SICHINT_CANONICAL,&order),"Read fresh normal App unpin persistence identity");
                if(order==0){++admitted;target.verify(retained.Get());require(!row.pinned,"Fresh actual normal App reloaded a stale pinned target");}
                retained.Reset();require(current(*session.app.Get(),freshSource),"Fresh native row comparison/release changed source");
            }
            require(admitted<=1,"Fresh normal App duplicates owned unpinned target");
            auto finalNative=inventory();originalUnchanged(original,finalNative,target);require(!targetPinned(finalNative,target),"Fresh normal App/native source repinned owned target");
            unsigned nativeMatches=0;for(const auto& row:finalNative)if(ownedPlace(row,target))++nativeMatches;
            require(admitted==1||nativeMatches==0,"Fresh App omitted an existing native unpinned target; fresh-source persistence coverage is incomplete");
            close(session);session.app.Reset();persistenceProved=true;
            std::cout<<"FreshAppPersistenceReceipt pinAndUnpinObserved=1 finalOwnedSourceRows="<<admitted<<" actualNativeOwnedPinned=0\n";
        }catch(const std::exception& error){outcome=1;failure=error.what();std::cerr<<"FAIL: genuine Hosted App pin persistence: "<<failure<<'\n';
            if(session.app)recordCallbacks(*session.app.Get(),"main-failure");}
        // Separate bounded recovery never changes the assertion outcome. Only
        // the owned GUID target may be unpinned, through the same real App UI.
        result.cleanupDeadline=GetTickCount64()+60000;ownedPinDeadline=result.cleanupDeadline;
        try {
            if(session.app&&session.app->window_)close(session);session.app.Reset();
            if(!baselineRead) {
                target.finish();std::cout<<"CleanupReceipt noProfilePinOrRecentMutationAttempted=1 exactOwnedEmptyFoldersRemoved=1 originalInventoryUnavailable=1\n";
                result.outcome=outcome?outcome:1;result.ownedFoldersRemoved=target.finished;result.appTeardownProved=!session.app;return result;
            }
            auto currentInventory=inventory();originalUnchanged(original,currentInventory,target);
            if(targetPinned(currentInventory,target)) {
                std::string recoveryDiagnostic;
                try {create(session,admission,desktop,target,layout);mutate(session,admission,desktop,target,false,false,true);}
                catch(const std::exception& error){recoveryDiagnostic=error.what();std::cerr<<"CleanupDeliveryDiagnostic: "<<recoveryDiagnostic<<'\n';}
                if(session.app&&session.app->window_)close(session);session.app.Reset();
                currentInventory=inventory();originalUnchanged(original,currentInventory,target);
                // A failed delivery assertion cannot conceal actual restoration
                // or turn the failed main persistence case into a success.
                if(!recoveryDiagnostic.empty())outcome=1;
            }
            require(!targetPinned(currentInventory,target),"Owned target remains pinned after bounded actual UI cleanup");
            recordInventory("restored",currentInventory,target);
            target.verify(target.item.Get());restored=true;
            std::cout<<"ProfileRestorationReceipt originalCompletePinnedInventoryRestored=1 ownedPinnedTargets=0 unownedTargetsInvoked=0\n";
            std::string folderCleanupFailure;bool foldersRemoved=false;
            try {currentInventory.clear();target.finish();foldersRemoved=true;}
            catch(const std::exception& error){folderCleanupFailure=error.what();}
            // Directory removal and native item Release may pump. Re-read the
            // complete native pinned inventory after those boundaries.
            restored=false;auto afterOwnedCleanup=inventory();exactOriginalPins(original,afterOwnedCleanup);restored=true;
            std::cout<<"CleanupReceipt exactOwnedEmptyFoldersRemoved="<<foldersRemoved<<" profileRestoredAfterCleanup=1\n";
            if(!folderCleanupFailure.empty())throw std::runtime_error("Owned folder cleanup failed after independently verified profile restoration: "+folderCleanupFailure);
        }catch(const std::exception& error) {
            std::cerr<<(restored?"CLEANUP_FAILURE profileRestored=1 ":"UNRECOVERED_PROFILE_FAILURE originalPinnedInventoryRestoration=UNPROVED ")<<error.what()<<'\n';
            outcome=restored?1:86;
            try{if(session.app&&session.app->window_)close(session);session.app.Reset();}catch(...){std::cerr<<"CleanupReceipt normalAppCloseUnproved=1\n";}
        }
        succeeded(drainStaWorkers(5000),"Final genuine normal App native workers did not drain");
        admission.normalAppAllowed(desktop);
        result.outcome=outcome;result.persistenceProved=persistenceProved;result.originalInventoryRead=baselineRead;
        result.ownedFoldersRemoved=target.finished;result.appTeardownProved=!session.app||!session.app->window_;
        for(const auto& pin:original)if(pin.pinned)result.originalPins.push_back({pin.fullPidl,pin.filesystem,pin.file});
        // Return only plain original authority. All App/provider/receipt/site/
        // inventory owners in this function retire before the caller continues.
        return result;
    }
    static void finalReadOnlyProfileAdmission(const Result& result) {
        require(result.originalInventoryRead&&result.appTeardownProved,"Original profile authority or actual normal App teardown is unproved");
        Inventory reconstructed;
        for(const auto& pin:result.originalPins) {
            NativePlace row;row.fullPidl=pin.fullPidl;row.filesystem=pin.filesystem;row.file=pin.file;row.pinned=true;
            row.fileIdentityKnown=pin.filesystem;row.fileIdentityStatus=pin.filesystem?S_OK:E_PENDING;
            succeeded(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(row.fullPidl.data()),IID_PPV_ARGS(&row.item)),
                "Reconstruct real original canonical pin identity from retained complete baseline PIDL");reconstructed.push_back(std::move(row));
        }
        auto actual=inventory();exactOriginalPins(reconstructed,actual);
        actual.clear();reconstructed.clear();checkOwnedPinDeadline();
        // A separate read-only validation apartment remains initialized until
        // the caller consumes this receipt; all its query objects retire first.
        std::cout<<"PostAppOLETeardownProfileReceipt completeOriginalPinnedInventoryRestored=1 originalCanonicalPins="<<result.originalPins.size()<<'\n';
    }
};
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=5||std::wcscmp(argv[1],L"--fresh-hosted-owned-pin-persistence")!=0)return 2;
    const bool finalOnly=std::wcscmp(argv[4],L"--final-destroy-only")==0;
    if(!finalOnly&&std::wcscmp(argv[4],L"--normal-flow")!=0)return 2;
    auto layout=explorer::RibbonLayout::Authored;
    if(std::wcscmp(argv[3],L"--installed")==0)layout=explorer::RibbonLayout::InstalledWindows10;
    else if(std::wcscmp(argv[3],L"--authored")!=0)return 2;
    // All authority is checked before normal App construction or profile writes.
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    try {
        ownedPinDeadline=GetTickCount64()+15000;HostedAdmission admission(argv[2],finalOnly?L"final-destroy-only":L"normal-flow");
        explorer::PrivateDesktop desktop;succeeded(desktop.initialize(),"Initialize owned isolated private desktop before COM");
        admission.normalAppAllowed(desktop);explorer::NativeApartmentOwner nativeApartment;
        const auto initialized=nativeApartment.initializeOle();succeeded(initialized,"Initialize genuine private App creator STA");
        explorer::HostedPinPersistenceFixture::Result observation;
        try {
            INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};
            require(InitCommonControlsEx(&controls)!=FALSE,"Initialize genuine private App controls");
            observation=explorer::HostedPinPersistenceFixture::run(admission,desktop,layout,finalOnly);
        }catch(const std::exception& error){std::cerr<<"UNRECOVERED_PROFILE_OR_TEARDOWN_FAILURE: "<<error.what()<<'\n';observation.outcome=86;}
        const auto drained=explorer::drainStaWorkers(5000);if(FAILED(drained)){
            std::cerr<<"UNRECOVERED_PROFILE_OR_TEARDOWN_FAILURE finalWorkerDrainHRESULT="<<static_cast<unsigned long>(drained)<<'\n';
            TerminateProcess(GetCurrentProcess(),86);std::_Exit(86);
        }
        const auto finished=nativeApartment.finish();if(FAILED(finished)) {
            std::cerr<<"UNRECOVERED_PROFILE_OR_TEARDOWN_FAILURE primaryApartmentFinishHRESULT="<<static_cast<unsigned long>(finished)<<'\n';
            TerminateProcess(GetCurrentProcess(),86);std::_Exit(86);
        }
        // Primary App STA and every owned App/receipt/provider/snapshot have
        // retired. Validate complete pinned authority in a new read-only STA.
        int result=observation.outcome;
        if(observation.originalInventoryRead&&observation.appTeardownProved) {
            std::promise<HRESULT> finishedValidation;auto finalValidation=finishedValidation.get_future();
            std::promise<std::string> validated;auto validation=validated.get_future();std::promise<void> retire;auto retired=retire.get_future();
            std::thread validator([&,desktopHandle=GetThreadDesktop(GetCurrentThreadId())] {
                ownedPinDeadline=observation.cleanupDeadline;
                const bool attached=SetThreadDesktop(desktopHandle)!=FALSE;
                explorer::NativeApartmentOwner validationApartment;
                const auto validationStatus=attached?validationApartment.initializeSta():HRESULT_FROM_WIN32(GetLastError());
                std::string error;
                try{succeeded(validationStatus,"Initialize separate private read-only final validation STA");
                    explorer::HostedPinPersistenceFixture::finalReadOnlyProfileAdmission(observation);}
                catch(const std::exception& failure){error=failure.what();}
                validated.set_value(error);retired.wait();
                finishedValidation.set_value(SUCCEEDED(validationStatus)?validationApartment.finish():validationStatus);
            });
            struct RetireValidation {std::promise<void>& signal;std::thread& task;
                ~RetireValidation(){try{signal.set_value();}catch(...){}if(task.joinable())task.join();}} retirement{retire,validator};
            // Synchronous native calls cannot be interrupted by the admission
            // budget; the existing CTest process bound remains the hard limit.
            const auto error=validation.get();ownedPinDeadline=observation.cleanupDeadline;
            if(!error.empty()){std::cerr<<"UNRECOVERED_PROFILE_FAILURE postAppOLETeardownRestoration=UNPROVED "<<error<<'\n';result=86;}
            else {
                if(result==86){result=1;std::cerr<<"LateProfileRestorationReceipt actualOriginalInventoryRestored=1 originalTestFailureRetained=1\n";}
                try{admission.normalAppAllowed(desktop);
                    if(result==0) {
                        require(observation.persistenceProved&&observation.ownedFoldersRemoved&&GetTickCount64()<observation.primaryDeadline,
                            "Original primary acceptance deadline or actual persistence/owned cleanup admission failed");
                        // Final PASS is deferred until actual validation apartment/thread retirement below.
                    }
                }catch(const std::exception& failure){std::cerr<<"FinalAdmissionFailure profileReadbackRestored=1 "<<failure.what()<<'\n';result=1;}
            }
            retire.set_value();const auto finalFinish=finalValidation.get();validator.join();
            if(FAILED(finalFinish)) {
                std::cerr<<"UNRECOVERED_PROFILE_OR_TEARDOWN_FAILURE validationApartmentFinishHRESULT="<<static_cast<unsigned long>(finalFinish)<<'\n';result=86;
            }
            if(result==0) {
                try {admission.normalAppAllowed(desktop);
                    require(observation.persistenceProved&&observation.ownedFoldersRemoved&&GetTickCount64()<observation.primaryDeadline,
                        "Actual validation teardown crossed the original acceptance deadline or isolation fence");
                    std::cout<<"PASS: genuine Hosted normal App "<<(finalOnly?"actual final-Destroy":"actual normal-flow")
                        <<" pin/unpin Invoke and fresh-source persistence; complete original pinned inventory restored after both App/OLE and validation teardown\n";
                }catch(const std::exception& failure){std::cerr<<"FinalRetirementAdmissionFailure "<<failure.what()<<'\n';result=1;}
            }
        }else {std::cerr<<"FinalProfileAdmission originalAuthorityOrAppTeardownUnproved=1\n";if(result==0)result=86;}
        return result;
    }catch(const std::exception& error){std::cerr<<"HOSTED_ADMISSION_OR_SETUP_REJECTED: "<<error.what()<<'\n';return 3;}
}
