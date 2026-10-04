#include "explorer/app.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/shell_operations.hpp"
#include "explorer/theme.hpp"
#include "explorer/worker_sta.hpp"
#include <UIRibbonPropertyHelpers.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <fstream>
#include <mutex>
#include <thread>

namespace explorer {
namespace {
struct SearchChoice { std::wstring label; std::wstring expression; };
const std::vector<SearchChoice>& kindChoices() {
    static thread_local const std::vector<SearchChoice> choices = [] {
        std::vector<SearchChoice> result;
        ComPtr<IPropertyDescription> description;
        ComPtr<IPropertyEnumTypeList> types;
        if (SUCCEEDED(PSGetPropertyDescription(PKEY_Kind, IID_PPV_ARGS(&description))) &&
            SUCCEEDED(description->GetEnumTypeList(IID_PPV_ARGS(&types)))) {
            UINT count = 0; types->GetCount(&count);
            for (UINT i = 0; i < count && i < 64; ++i) {
                ComPtr<IPropertyEnumType> type;
                PWSTR title = nullptr;
                PROPVARIANT value{};
                if (SUCCEEDED(types->GetAt(i, IID_PPV_ARGS(&type))) &&
                    SUCCEEDED(type->GetDisplayText(&title)) && SUCCEEDED(type->GetValue(&value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal && title) {
                    const std::wstring canonical = value.pwszVal;
                    if (canonical.find_first_of(L"\"\\") == std::wstring::npos)
                        result.push_back({title, L"System.Kind:=\"" + canonical + L"\""});
                }
                CoTaskMemFree(title); PropVariantClear(&value);
            }
        }
        return result;
    }();
    return choices;
}

const std::vector<SearchChoice>& dateChoices() {
    static const std::vector<SearchChoice> choices{
        {L"Today",L"System.DateModified:System.StructuredQueryType.DateTime#Today"},
        {L"Yesterday",L"System.DateModified:System.StructuredQueryType.DateTime#Yesterday"},
        {L"This week",L"System.DateModified:System.StructuredQueryType.DateTime#ThisWeek"},
        {L"Last week",L"System.DateModified:System.StructuredQueryType.DateTime#LastWeek"},
        {L"This month",L"System.DateModified:System.StructuredQueryType.DateTime#ThisMonth"},
        {L"Last month",L"System.DateModified:System.StructuredQueryType.DateTime#LastMonth"},
        {L"This year",L"System.DateModified:System.StructuredQueryType.DateTime#ThisYear"},
        {L"Last year",L"System.DateModified:System.StructuredQueryType.DateTime#LastYear"}};
    return choices;
}
const std::vector<SearchChoice>& sizeChoices() {
    static const std::vector<SearchChoice> choices{
        {L"Empty (0 KB)",L"System.Size:System.Size#Empty"},
        {L"Tiny (0–16 KB)",L"System.Size:System.Size#Tiny"},
        {L"Small (16 KB–1 MB)",L"System.Size:System.Size#Small"},
        {L"Medium (1–128 MB)",L"System.Size:System.Size#Medium"},
        {L"Large (128 MB–1 GB)",L"System.Size:System.Size#Large"},
        {L"Huge (1–4 GB)",L"System.Size:System.Size#Huge"},
        {L"Gigantic (over 4 GB)",L"System.Size:System.Size#Gigantic"}};
    return choices;
}
const std::array<SearchChoice,4>& otherPropertyChoices() {
    static thread_local const std::array<SearchChoice,4> choices=[] {
        std::array<SearchChoice,4> result{{
            {L"Folder path",L"System.ItemFolderPathDisplay:"},
            {L"Name",L"System.FileName:"},
            {L"Tags",L"System.Keywords:"},
            {L"File extension",L"System.FileExtension:"}}};
        // The installed public command provider owns the localized labels and
        // native order; the canonical tokens remain independent of language.
        std::vector<NamespaceSubcommandMetadata> native;
        if(SUCCEEDED(namespaceCommandChildren(L"Windows.SearchFilterMoreProperties",nullptr,nullptr,&native))&&native.size()==result.size())
            for(size_t index=0;index<result.size();++index)if(!native[index].label.empty())result[index].label=native[index].label;
        return result;
    }();
    return choices;
}
std::wstring nameOf(IShellItem* item, SIGDN format) {
    PWSTR raw = nullptr;
    if (!item || FAILED(item->GetDisplayName(format, &raw))) return {};
    std::wstring result = raw; CoTaskMemFree(raw); return result;
}
bool pumpVisual(const std::function<bool()>& ready, DWORD milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (ready()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
    return ready();
}
}

AppCommandContext ExplorerApp::commandContext() const {
    AppCommandContext context;
    context.folderView = folderView_ != nullptr;
    context.navigating = navigating_;
    context.physicalDirectory = physicalDirectory_;
    context.library = library_.valid();
    context.writableLibrary = library_.valid() && library_.writable();
    context.search = searchActive_;
    context.searchBackground = searchBackground_;
    context.detailsView = preferences_.view == ViewMode::Details;
    context.archive = archiveFolder_ || selectionArchive_;
    context.driveRoot = namespaceActions_.facts().driveRoot;
    context.computer = StrStrIW(currentLocation_.c_str(), L"20D04FE0") != nullptr;
    context.network=namespaceNetwork_;
    context.selectionCount = selectionCount_;
    context.selectionAttributes = selectionAttributes_;
    return context;
}

HRESULT ExplorerApp::applyNavigationOptions() {
    if (!browser_ || !preferences_.navigationPane) return S_FALSE;
    ComPtr<INameSpaceTreeControl2> tree;
    auto hr = IUnknown_QueryService(browser_.Get(), SID_SNavigationPane, IID_PPV_ARGS(&tree));
    if (FAILED(hr) && view_) hr = IUnknown_QueryService(view_.Get(), SID_SNavigationPane, IID_PPV_ARGS(&tree));
    if (FAILED(hr) && view_) {
        ComPtr<IObjectWithSite> located;
        ComPtr<IServiceProvider> frame;
        if (SUCCEEDED(view_.As(&located)) && SUCCEEDED(located->GetSite(IID_PPV_ARGS(&frame))))
            hr = frame->QueryService(SID_SNavigationPane,IID_PPV_ARGS(&tree));
    }
    if (FAILED(hr)) return hr;
    hr = tree->SetControlStyle2(NSTCS2_DISPLAYPINNEDONLY, showAllFolders_ ? NSTCS2_DEFAULT : NSTCS2_DISPLAYPINNEDONLY);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> libraries;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Libraries,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&libraries)))) {
        ComPtr<IShellItemArray> roots; DWORD count=0; bool present=false;
        if (SUCCEEDED(tree->GetRootItems(&roots)) && roots && SUCCEEDED(roots->GetCount(&count))) {
            for (DWORD i=0;i<count;++i) { ComPtr<IShellItem> root; int order=1;
                if (SUCCEEDED(roots->GetItemAt(i,&root)) && SUCCEEDED(root->Compare(libraries.Get(),SICHINT_CANONICAL,&order)) && !order) present=true;
            }
            if (showLibraries_ && !present) hr=tree->AppendRoot(libraries.Get(),SHCONTF_FOLDERS,NSTCRS_VISIBLE,nullptr);
            if (!showLibraries_ && present) hr=tree->RemoveRoot(libraries.Get());
            if(FAILED(hr))return hr;
        }
    }
    KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();
    if(expandCurrent_) {
        ComPtr<IShellItem> current;
        hr=currentFolder(current);if(FAILED(hr))return hr;
        RECT existing{};
        if(SUCCEEDED(tree->GetItemRect(current.Get(),&existing)))return tree->EnsureItemVisible(current.Get());
        std::vector<ComPtr<IShellItem>> chain;
        for(unsigned depth=0;current&&depth<64;++depth) {
            chain.push_back(current);ComPtr<IShellItem> parent;
            if(FAILED(current->GetParent(&parent)))break;
            int same=1;if(SUCCEEDED(parent->Compare(current.Get(),SICHINT_CANONICAL,&same))&&!same)break;
            current=std::move(parent);
        }
        std::reverse(chain.begin(),chain.end());
        ComPtr<IShellItemArray> roots;DWORD rootCount=0;
        hr=tree->GetRootItems(&roots);if(FAILED(hr))return hr;
        hr=roots->GetCount(&rootCount);if(FAILED(hr))return hr;
        size_t first=chain.size();
        for(size_t index=0;index<chain.size();++index) {
            for(DWORD r=0;r<rootCount;++r) {
                ComPtr<IShellItem> root;int same=1;
                if(SUCCEEDED(roots->GetItemAt(r,&root))&&SUCCEEDED(root->Compare(chain[index].Get(),SICHINT_CANONICAL,&same))&&!same)first=index;
            }
        }
        // A pinned canonical ancestor can exist below a Quick access root.
        if(first==chain.size())for(size_t index=chain.size();index-->0;) {
            RECT visible{};if(SUCCEEDED(tree->GetItemRect(chain[index].Get(),&visible))){first=index;break;}
        }
        if(first==chain.size())return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        navigationExpansion_.assign(chain.begin()+static_cast<std::ptrdiff_t>(first),chain.end());
        navigationTree_=tree;navigationExpansionIndex_=0;navigationExpansionRequested_.reset();navigationExpansionGeneration_=navigationCount_;
        navigationExpansionStatus_=S_OK;
        navigationExpansionDeadline_=GetTickCount64()+5000;
        SetTimer(window_,3,15,nullptr);advanceNavigationExpansion();
    }
    return hr;
}

void ExplorerApp::advanceNavigationExpansion() {
    if(!navigationTree_||navigationExpansion_.empty()||closing_||navigating_||
        navigationExpansionGeneration_!=navigationCount_||GetTickCount64()>=navigationExpansionDeadline_) {
        KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();return;
    }
    auto& item=navigationExpansion_[navigationExpansionIndex_];
    // A hidden native root and an off-screen child can be materialized without
    // a drawable rectangle. Item state, rather than geometry, proves presence.
    NSTCITEMSTATE state{};
    if(FAILED(navigationTree_->GetItemState(item.Get(),NSTCIS_EXPANDED,&state)))return;
    if(navigationExpansionIndex_+1==navigationExpansion_.size()) {
        navigationTree_->EnsureItemVisible(item.Get());
        KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();return;
    }
    if(!(state&NSTCIS_EXPANDED)&&navigationExpansionRequested_!=navigationExpansionIndex_) {
        navigationTree_->EnsureItemVisible(item.Get());
        navigationExpansionStatus_=expandNativeTreeItem(navigationTree_.Get(),item.Get(),window_);
        if(FAILED(navigationExpansionStatus_)&&navigationExpansionStatus_!=E_PENDING)return;
        navigationExpansionRequested_=navigationExpansionIndex_;
    }
    NSTCITEMSTATE child{};
    if(SUCCEEDED(navigationTree_->GetItemState(navigationExpansion_[navigationExpansionIndex_+1].Get(),NSTCIS_EXPANDED,&child))) {
        ++navigationExpansionIndex_;
        navigationExpansionRequested_.reset();
    }
}

void ExplorerApp::cancelCommandStates() {
    if(commandRefreshActive_) {commandStatesCancelPending_=true;return;}
    cancelCommandStatesImpl();
}
void ExplorerApp::cancelCommandStatesImpl() {
    if(window_)KillTimer(window_,4);
    if(selectionStateBatch_)selectionStateBatch_->cancel();
    selectionStateBatch_.reset();selectionStateBindings_.clear();
    for(auto& [id,task]:commandStateTasks_) { (void)id;task->cancel(); }
    commandStateTasks_.clear();
}

void ExplorerApp::startPendingCommandStates() {
    if(closing_)return;
    if(!selectionStateBatch_) {
        std::vector<AppCommandCapability> batch;
        for(const auto& [id,capability]:commandCapabilities_)
            if(capability.status==E_PENDING&&!capability.selectionVerbs.empty()&&
               capability.binding.scope==NamespaceMenuScope::Selection&&!(id==Extract&&archiveTargetValid_)&&
               !commandStateTasks_.contains(id))batch.push_back(capability);
        if(!batch.empty()) {
            const auto status=startAppSelectionStateBatch(namespaceActions_,batch,&selectionStateBatch_,&selectionStateBindings_);
            if(FAILED(status)&&status!=HRESULT_FROM_WIN32(ERROR_BUSY))
                for(const auto& capability:batch)completeCommandState(capability.binding.command,status,nullptr);
        }
    }
    for(auto& [id,capability]:commandCapabilities_) {
        if(capability.status!=E_PENDING)continue;
        // One native selection menu owns every marked default-menu state and
        // alias in this generation. Busy batches retry without per-leaf jobs.
        if(!capability.selectionVerbs.empty()&&capability.binding.scope==NamespaceMenuScope::Selection&&
           !(id==Extract&&archiveTargetValid_))continue;
        if(commandStateTasks_.contains(id))continue;
        std::unique_ptr<NamespaceCommandStateTask> task;
        auto& actions=id==Extract&&archiveTargetValid_?archiveActions_:
            capability.binding.scope==NamespaceMenuScope::Background?backgroundActions_:namespaceActions_;
        const auto hr=startAppCommandStateTask(actions,id,&task);
        if(SUCCEEDED(hr))commandStateTasks_.emplace(id,std::move(task));
        else if(hr!=HRESULT_FROM_WIN32(ERROR_BUSY)) {capability.status=hr;capability.enabled=false;}
    }
    const bool pending=std::any_of(commandCapabilities_.begin(),commandCapabilities_.end(),
        [](const auto& entry){return entry.second.status==E_PENDING;});
    if(pending)SetTimer(window_,4,50,nullptr);else KillTimer(window_,4);
}

void ExplorerApp::completeCommandState(UINT command,HRESULT status,const NamespaceCommandState* native) {
    const auto found=commandCapabilities_.find(command);
    if(found==commandCapabilities_.end())return;
    auto& capability=found->second;capability.status=status;
    if(SUCCEEDED(status)&&native)capability.native=*native;
    capability.enabled=SUCCEEDED(status)&&native&&native->enabled();
    capability.checked=SUCCEEDED(status)&&native&&native->checked();
    if(command==Undo)undoAvailable_=capability.enabled;
    if(command==Redo)redoAvailable_=capability.enabled;
    // Slow state may establish the provider's association/site context. An
    // earlier unavailable child snapshot no longer describes that context.
    ribbonCommandChildren_.erase(command);ribbonCommandPaths_.erase(command);
    if(command==RibbonNewMenu)newItemTypes_.reset();
    if(command==RibbonExtractToGallery)extractDestinations_.reset();
    ribbon_.invalidateState(command);ribbon_.invalidateItems(command);
}

void ExplorerApp::pollCommandStates() {
    if(commandRefreshActive_) {deferCommandRefresh();return;}
    if(closing_||navigating_) {cancelCommandStates();return;}
    if(GetClipboardSequenceNumber()!=clipboardSequence_) {cancelCommandStates();namespaceDirty_=true;updateCommands();return;}
    if(selectionStateDirty_||namespaceDirty_) {cancelCommandStates();updateCommands();return;}
    CommandRefreshScope scope(*this);
    if(selectionStateBatch_) {
        std::vector<NamespaceSelectionVerbState> states;
        const auto status=selectionStateBatch_->pollSelectionVerbBatch(&states);
        if(status!=E_PENDING) {
            auto bindings=std::move(selectionStateBindings_);
            selectionStateBindings_.clear();selectionStateBatch_.reset();
            for(const auto& binding:bindings) {
                NamespaceCommandState native;
                const auto read=SUCCEEDED(status)?applyAppSelectionStateBatch(binding,states,&native):status;
                completeCommandState(binding.command,read,SUCCEEDED(read)?&native:nullptr);
            }
        }
    }
    for(auto task=commandStateTasks_.begin();task!=commandStateTasks_.end();) {
        NamespaceCommandState native;
        const auto hr=task->second->poll(&native);
        if(hr==E_PENDING) {++task;continue;}
        completeCommandState(task->first,hr,SUCCEEDED(hr)?&native:nullptr);
        task=commandStateTasks_.erase(task);
    }
    startPendingCommandStates();
}

void ExplorerApp::fitBreadcrumbs(int width) {
    if (!breadcrumbs_ || width<=0) return;
    width=std::max(px(18),width-px(11));
    SendMessageW(breadcrumbs_,TB_HIDEBUTTON,9021,MAKELONG(TRUE,0));
    for (size_t i=0;i<breadcrumbsPidls_.size();++i)
        SendMessageW(breadcrumbs_,TB_HIDEBUTTON,BreadcrumbFirst+i,MAKELONG(FALSE,0));
    // Adding buttons, changing the toolbar font or its theme invalidates
    // previously assigned widths. Size all buttons after those operations.
    const auto dc=GetDC(breadcrumbs_);
    const auto nativeFont=reinterpret_cast<HFONT>(SendMessageW(breadcrumbs_,WM_GETFONT,0,0));
    const auto selectedFont=nativeFont?nativeFont:font_;
    const auto oldFont=dc&&selectedFont?SelectObject(dc,selectedFont):nullptr;
    std::vector<int> widths;
    for(size_t index=0;index<breadcrumbsPidls_.size();++index) {
        std::array<wchar_t,512> label{};
        TBBUTTONINFOW text{sizeof(text)};text.dwMask=TBIF_TEXT;text.pszText=label.data();text.cchText=static_cast<int>(label.size());
        SendMessageW(breadcrumbs_,TB_GETBUTTONINFOW,BreadcrumbFirst+index,reinterpret_cast<LPARAM>(&text));
        SIZE extent{};if(dc)GetTextExtentPoint32W(dc,label.data(),static_cast<int>(wcslen(label.data())),&extent);
        widths.push_back(index==0&&breadcrumbsPidls_.size()>1?px(34):std::min(px(220),static_cast<int>(extent.cx)+px(index==0?42:24)));
    }
    if(oldFont)SelectObject(dc,oldFont);if(dc)ReleaseDC(breadcrumbs_,dc);
    if(!widths.empty())widths.back()=std::min(widths.back(),std::max(px(8),width-px(18)-(widths.size()>1?widths.front():0)));
    int used=px(18);for(const auto value:widths)used+=value;
    for(size_t index=1;index+1<widths.size()&&used>width;++index) {
        SendMessageW(breadcrumbs_,TB_HIDEBUTTON,BreadcrumbFirst+index,MAKELONG(TRUE,0));used-=widths[index];
    }
    if(used<width) {
        TBBUTTONINFOW spacer{sizeof(spacer)};spacer.dwMask=TBIF_SIZE|TBIF_IMAGE;
        spacer.cx=static_cast<WORD>(std::min<int>(width-used,65535));
        spacer.iImage=spacer.cx;
        SendMessageW(breadcrumbs_,TB_SETBUTTONINFOW,9021,reinterpret_cast<LPARAM>(&spacer));
        SendMessageW(breadcrumbs_,TB_HIDEBUTTON,9021,MAKELONG(FALSE,0));
    }
    // A separator image-width update also invalidates native button widths.
    // Apply the final widths after changing the spacer and visibility.
    for(size_t index=0;index<widths.size();++index) {
        TBBUTTONINFOW size{sizeof(size)};size.dwMask=TBIF_SIZE;size.cx=static_cast<WORD>(widths[index]);
        SendMessageW(breadcrumbs_,TB_SETBUTTONINFOW,BreadcrumbFirst+index,reinterpret_cast<LPARAM>(&size));
    }
    TBBUTTONINFOW addressSize{sizeof(addressSize)};addressSize.dwMask=TBIF_SIZE;addressSize.cx=static_cast<WORD>(px(18));
    SendMessageW(breadcrumbs_,TB_SETBUTTONINFOW,AddressList,reinterpret_cast<LPARAM>(&addressSize));
}

void ExplorerApp::initializeBreadcrumbDrop() {
    if (headless_ || !breadcrumbs_ || !view_ || navigating_) return;
    if (breadcrumbDrop_) { breadcrumbDrop_->revokeWindow(); breadcrumbDrop_.Reset(); }
    BreadcrumbDropOptions options;
    options.site=view_;
    options.hitTest=[this](POINTL screen,IShellItem** item)->HRESULT {
        if (!item) return E_POINTER;
        *item=nullptr;
        if (closing_ || navigating_ || addressEditing_) return S_FALSE;
        POINT point{screen.x,screen.y};
        if (!ScreenToClient(breadcrumbs_,&point)) return HRESULT_FROM_WIN32(GetLastError());
        const auto index=SendMessageW(breadcrumbs_,TB_HITTEST,0,reinterpret_cast<LPARAM>(&point));
        TBBUTTON button{};
        if (index<0 || !SendMessageW(breadcrumbs_,TB_GETBUTTON,index,reinterpret_cast<LPARAM>(&button)) ||
            button.idCommand<BreadcrumbFirst || static_cast<size_t>(button.idCommand-BreadcrumbFirst)>=breadcrumbsPidls_.size()) return S_FALSE;
        return SHCreateItemFromIDList(breadcrumbsPidls_[button.idCommand-BreadcrumbFirst].get(),IID_PPV_ARGS(item));
    };
    ComPtr<BreadcrumbDropTarget> target;
    auto hr=BreadcrumbDropTarget::create(breadcrumbs_,options,&target);
    if (SUCCEEDED(hr)) hr=target->registerWindow();
    if (SUCCEEDED(hr)) breadcrumbDrop_=std::move(target);
}

void ExplorerApp::beginBreadcrumbMenu(UINT command) {
    if (headless_ || navigating_ || command<BreadcrumbFirst || command-BreadcrumbFirst>=breadcrumbsPidls_.size()) return;
    if (breadcrumbTask_) { breadcrumbTask_->cancel(); breadcrumbTask_.reset(); }
    const auto index=command-BreadcrumbFirst;
    ComPtr<IShellItem> parent,selected;
    auto hr=SHCreateItemFromIDList(breadcrumbsPidls_[index].get(),IID_PPV_ARGS(&parent));
    if (SUCCEEDED(hr) && index+1<breadcrumbsPidls_.size())
        SHCreateItemFromIDList(breadcrumbsPidls_[index+1].get(),IID_PPV_ARGS(&selected));
    if (FAILED(hr)) { showError(hr,L"Read breadcrumb folders"); return; }
    RECT button{};
    if (!SendMessageW(breadcrumbs_,TB_GETRECT,command,reinterpret_cast<LPARAM>(&button))) return;
    MapWindowPoints(breadcrumbs_,nullptr,reinterpret_cast<POINT*>(&button),2);
    breadcrumbMenuPoint_={button.left,button.bottom};
    breadcrumbGeneration_=navigationCount_;
    BreadcrumbEnumerationOptions options; options.showHidden=preferences_.showHidden;
    hr=BreadcrumbEnumerationTask::start(parent.Get(),selected.Get(),options,&breadcrumbTask_);
    if (FAILED(hr)) { showError(hr,L"Read breadcrumb folders"); return; }
    SetTimer(window_,2,15,nullptr);
    pollBreadcrumbMenu();
}

void ExplorerApp::pollBreadcrumbMenu() {
    if (!breadcrumbTask_ || headless_ || closing_) { KillTimer(window_,2); return; }
    BreadcrumbSnapshot snapshot;
    const auto hr=breadcrumbTask_->poll(&snapshot);
    if (hr==E_PENDING) return;
    breadcrumbTask_.reset(); KillTimer(window_,2);
    if (navigating_ || breadcrumbGeneration_!=navigationCount_) return;
    if (FAILED(hr)) { showError(hr,L"Read breadcrumb folders"); return; }
    const auto menu=CreatePopupMenu();
    if (!menu) return;
    constexpr UINT first=40000;
    for (UINT i=0;i<snapshot.children.size();++i) {
        AppendMenuW(menu,MF_STRING,first+i,snapshot.children[i].label.c_str());
        if (snapshot.children[i].selected) SetMenuDefaultItem(menu,first+i,FALSE);
    }
    if (snapshot.children.empty()) AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"No subfolders");
    if (!snapshot.complete) AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"Additional folders are still unavailable");
    const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN,
        breadcrumbMenuPoint_.x,breadcrumbMenuPoint_.y,0,window_,nullptr);
    DestroyMenu(menu);
    if (selected>=first && selected-first<snapshot.children.size() && !navigating_ && breadcrumbGeneration_==navigationCount_)
        showError(browser_->BrowseToObject(snapshot.children[selected-first].item.Get(),SBSP_ABSOLUTE),L"Open breadcrumb folder");
}

RibbonCommandState ExplorerApp::ribbonState(UINT command) {
    RibbonCommandState state;
    if (command == RibbonShareGallery) return ribbonState(RibbonSpecificPeople);
    if (command >= RibbonHomeTab && command <= RibbonDiscImageTab) return state;
    if (command >= ViewFirst && command <= ViewLast) {
        state.checked = static_cast<UINT>(preferences_.view) == command - ViewFirst;
        state.enabled = folderView_ != nullptr; return state;
    }
    if (command == RibbonLayoutGallery) {
        state.selectedIndex = static_cast<UINT>(preferences_.view);
        state.enabled = folderView_ != nullptr; return state;
    }
    if(command==NewFolder&&librariesRoot_) {
        state.enabled=nativeLibraryFactoryReady_&&!navigating_;
        if(newItemTypes_&&!newItemTypes_->entries().empty())state.label=newItemTypes_->entries().front().label;
        return state;
    }
    const auto capability = commandCapabilities_.find(command);
    if (capability != commandCapabilities_.end() && capability->second.binding.route != AppCommandRoute::Host) {
        state.enabled = capability->second.enabled;
        state.checked = capability->second.checked;
        return state;
    }
    const auto binding = appCommandBinding(command);
    if (binding && !appCommandApplicable(*binding, commandContext())) { state.enabled = false; return state; }
    if (binding && binding->route != AppCommandRoute::Host) { state.enabled = false; return state; }
    const bool selected = selectionCount_ != 0;
    switch (command) {
    case Copy: case CopyPath: case RibbonCopyMenu: case CopyTo:
        state.enabled = selected && (selectionAttributes_ & SFGAO_CANCOPY); break;
    case Cut: case RibbonMoveMenu: case MoveTo:
        state.enabled = selected && (selectionAttributes_ & SFGAO_CANMOVE); break;
    case Delete: case PermanentDelete: case RibbonDeleteMenu:
        state.enabled = selected && (selectionAttributes_ & SFGAO_CANDELETE); break;
    case Rename: state.enabled = selectionCount_ == 1 && (selectionAttributes_ & SFGAO_CANRENAME); break;
    case Properties: case RibbonPropertiesMenu:
        state.enabled = !selected || (selectionAttributes_ & SFGAO_HASPROPSHEET); break;
    case Paste: case PasteShortcut:
        state.enabled = physicalDirectory_ && !navigating_ && clipboardFiles_; break;
    case Open: case Edit: case Print: case RibbonOpenMenu: case Zip: state.enabled = selected; break;
    case Extract: state.enabled = static_cast<UINT>(ribbonContexts_) & static_cast<UINT>(RibbonContext::Compressed); break;
    case Sharing: state.enabled = selectionShareable_; break;
    case Undo: state.enabled = undoAvailable_; break;
    case Redo: state.enabled = redoAvailable_; break;
    case NewFolder: case NewItems: case RibbonNewMenu:
        state.enabled=folderView_&&!navigating_;break;
    case NewText: case NewShortcut:
    case Terminal: case RibbonPowerShellMenu: case RibbonPowerShellAdmin:
        state.enabled = physicalDirectory_ && !navigating_; break;
    case ColumnsMenu: case SizeColumns: {
        state.enabled = folderView_ && preferences_.view == ViewMode::Details;
        break;
    }
    case SelectAll: case Invert: state.enabled = folderView_ != nullptr; break;
    case SelectNone: state.enabled = selected; break;
    case HideSelected:
        state.enabled = selected && selectionFilesystem_;
        state.label = selectionHidden_ ? L"Unhide selected items" : L"Hide selected items"; break;
    case NavigationPane: state.checked = preferences_.navigationPane; break;
    case PreviewPane: state.checked = preferences_.previewPane; break;
    case DetailsPane: state.checked = preferences_.detailsPane; break;
    case Checkboxes: state.checked = checkboxes_; break;
    case Extensions: state.checked = preferences_.showExtensions; break;
    case HiddenItems: state.checked = preferences_.showHidden; break;
    case Collapse: state.checked = preferences_.ribbonCollapsed; break;
    case Fullscreen: state.checked = fullscreen_; break;
    case SortAscending: state.checked = ascending_; break;
    case SortDescending: state.checked = !ascending_; break;
    case SearchCurrent: state.checked = searchActive_ && !searchRecursive_; state.enabled = searchActive_; break;
    case SearchSubfolders: state.checked = searchActive_ && searchRecursive_; state.enabled = searchActive_; break;
    case RecentSearches: case RibbonClearSearchHistory: state.enabled = !recentSearches_.empty(); break;
    case SaveSearch: case CloseSearch: case SearchKindMenu: case SearchDateMenu: case SearchSizeMenu:
    case RibbonSearchOtherProperties: case RibbonSearchAdvancedMenu:
        state.enabled = searchActive_; break;
    case OpenFileLocation: state.enabled = searchBackground_ && selectionCount_ > 0; break;
    case LibraryLocations: state.enabled = library_.valid(); break;
    case IncludeLibraryFolder: case LibraryDefault: case LibraryOptimize: case RibbonLibraryOptimizeMenu:
    case RibbonResetLibrary: case RibbonLibraryChangeIcon: case RibbonLibraryShowInNavigation:
        state.enabled = library_.valid() && library_.writable();
        if (command == RibbonLibraryShowInNavigation && library_.valid()) {
            LIBRARYOPTIONFLAGS flags{};
            if (SUCCEEDED(library_.native()->GetOptions(&flags))) state.checked = !(flags & LOF_PINNEDTONAVPANE) ? false : true;
        }
        break;
    case RibbonExpandToCurrent: state.checked = expandCurrent_; break;
    case RibbonShowAllFolders: state.checked = showAllFolders_; break;
    case RibbonShowLibraries: state.checked = showLibraries_; break;
    default: break;
    }
    if (capability != commandCapabilities_.end()) state.enabled &= capability->second.enabled;
    return state;
}

struct ExplorerApp::FrequentPlacesState {
    struct Value {std::vector<BYTE> identity;std::wstring label;std::wstring description;bool pinned=false;};
    std::mutex mutex;std::vector<Value> values;
    std::atomic<bool> done=false,cancelled=false;
    HRESULT status=E_PENDING;
};

void ExplorerApp::cancelFrequentPlaces() {
    if(window_)KillTimer(window_,5);
    if(frequentPlacesTask_)frequentPlacesTask_->cancelled=true;
    frequentPlacesTask_.reset();
}

void ExplorerApp::refreshFrequentPlaces() {
    if(frequentPlacesTask_||closing_||!window_)return;
    auto task=std::make_shared<FrequentPlacesState>();
    std::unique_ptr<StaWorkerLease> worker;
    const auto prepared=StaWorkerLease::prepare(&worker);
    if(FAILED(prepared))return;
    frequentPlacesTask_=task;
    try {std::thread([task,worker=std::move(worker)] {
        HRESULT status=worker->attach();
        bool initialized=false;
        std::vector<FrequentPlacesState::Value> values;
        try {
        if(SUCCEEDED(status)) {status=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);initialized=SUCCEEDED(status);}
        if(SUCCEEDED(status)&&!task->cancelled) {
            ComPtr<IShellItem> home;ComPtr<IShellFolder> folder;ComPtr<IEnumIDList> items;
            status=SHCreateItemFromParsingName(L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",nullptr,IID_PPV_ARGS(&home));
            if(SUCCEEDED(status))status=home->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(&folder));
            if(SUCCEEDED(status))status=folder->EnumObjects(nullptr,SHCONTF_FOLDERS,&items);
            const auto end=GetTickCount64()+3000;
            while(SUCCEEDED(status)&&items&&!task->cancelled&&values.size()<10&&GetTickCount64()<end) {
                PIDLIST_RELATIVE raw=nullptr;
                const auto next=items->Next(1,&raw,nullptr);
                if(next!=S_OK) {if(FAILED(next))status=next;break;}
                Pidl relative(raw);ComPtr<IShellItem> item;ComPtr<IShellItem2> extended;
                if(FAILED(SHCreateItemWithParent(nullptr,folder.Get(),relative.get(),IID_PPV_ARGS(&item))))continue;
                FrequentPlacesState::Value value;
                value.label=nameOf(item.Get(),SIGDN_NORMALDISPLAY);
                value.description=nameOf(item.Get(),SIGDN_DESKTOPABSOLUTEPARSING);
                PIDLIST_ABSOLUTE identity=nullptr;
                if(value.label.empty()||FAILED(SHGetIDListFromObject(item.Get(),&identity)))continue;
                Pidl owned(identity);const auto bytes=ILGetSize(identity);
                if(!bytes||bytes>65536)continue;
                value.identity.assign(reinterpret_cast<const BYTE*>(identity),reinterpret_cast<const BYTE*>(identity)+bytes);
                ComPtr<IPropertyStore> properties;PROPVARIANT pin{};BOOL pinned=FALSE;
                if(SUCCEEDED(item.As(&extended))&&SUCCEEDED(extended->GetPropertyStore(GPS_FASTPROPERTIESONLY|GPS_BESTEFFORT,IID_PPV_ARGS(&properties)))&&
                   SUCCEEDED(properties->GetValue(PKEY_Home_IsPinned,&pin))&&SUCCEEDED(PropVariantToBoolean(pin,&pinned)))value.pinned=pinned!=FALSE;
                PropVariantClear(&pin);values.push_back(std::move(value));
            }
        }
        }catch(const std::bad_alloc&) {status=E_OUTOFMEMORY;}
         catch(...) {status=E_FAIL;}
        if(initialized)CoUninitialize();
        const auto finished=worker->finish();
        if(FAILED(finished))status=finished;
        {std::lock_guard lock(task->mutex);task->status=status;task->values=std::move(values);}
        task->done=true;
    }).detach();}catch(...) {frequentPlacesTask_.reset();return;}
    SetTimer(window_,5,30,nullptr);
}

void ExplorerApp::pollFrequentPlaces() {
    if(!frequentPlacesTask_||closing_) {cancelFrequentPlaces();return;}
    if(!frequentPlacesTask_->done)return;
    auto task=std::move(frequentPlacesTask_);KillTimer(window_,5);
    std::lock_guard lock(task->mutex);
    if(SUCCEEDED(task->status)&&!task->cancelled) {
        std::vector<FrequentPlace> places;
        for(auto& value:task->values) {
            ComPtr<IShellItem> item;
            if(SUCCEEDED(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(value.identity.data()),IID_PPV_ARGS(&item))))
                places.push_back({std::move(item),std::move(value.label),std::move(value.description),value.pinned});
        }
        frequentPlaces_=std::move(places);frequentPlacesReadAt_=GetTickCount64();
        ribbon_.invalidate(RibbonFrequentPlaces);
    }
}

HRESULT ExplorerApp::pinFrequentPlace(UINT index,bool pinned) {
    if(index>=displayedFrequentPlaces_.size())return E_INVALIDARG;
    const auto& place=displayedFrequentPlaces_[index];
    if(place.pinned==pinned)return S_FALSE;
    if(headless_)return E_ACCESSDENIED;
    ComPtr<IShellItemArray> items;auto hr=SHCreateShellItemArrayFromShellItem(place.item.Get(),IID_PPV_ARGS(&items));
    NativeContextMenu menu;std::vector<ContextMenuEntry> entries;
    if(SUCCEEDED(hr))hr=menu.createSelection(window_,items.Get(),view_.Get());
    if(SUCCEEDED(hr))hr=menu.enumerate(entries,false);
    if(FAILED(hr))return hr;
    const auto verb=pinned?L"pintohome":L"unpinfromhome";
    const auto entry=std::find_if(entries.begin(),entries.end(),[verb](const auto& candidate){return candidate.canonicalVerb==verb&&candidate.enabled()&&!candidate.submenu;});
    if(entry==entries.end())return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    activeContextMenu_=&menu;hr=menu.invoke(entry->id);activeContextMenu_=nullptr;
    if(SUCCEEDED(hr)){displayedFrequentPlaces_[index].pinned=pinned;frequentPlacesReadAt_=0;refreshFrequentPlaces();namespaceDirty_=true;updateCommands();}
    return hr;
}

std::vector<RibbonItem> ExplorerApp::ribbonItems(UINT command) {
    std::vector<RibbonItem> result;
    if(commandRefreshActive_) {
        commandItemsRefreshPending_=true;deferCommandRefresh();return result;
    }
    CommandRefreshScope refresh(*this);
    if(command==RibbonNewMenu) {
        updateNamespaceImpl();
        if(!newItemTypes_&&ribbonState(RibbonNewMenu).enabled)backgroundActions_.queryCommandChildren(L"Windows.newitem",&newItemTypes_,NamespaceMenuScope::Background);
        if(newItemTypes_)for(UINT index=0;index<newItemTypes_->entries().size();++index) {
            const auto& entry=newItemTypes_->entries()[index];
            result.push_back({index,entry.label,false,entry.icon,entry.description,
                SUCCEEDED(entry.stateStatus)&&!(entry.state&(ECS_DISABLED|ECS_HIDDEN)),(entry.state&ECS_CHECKED)!=0});
        }
    } else if(command==RibbonExtractToGallery) {
        updateNamespaceImpl();
        if(!archiveFolder_&&!selectionArchive_)return result;
        if(!extractDestinations_)namespaceActions_.queryCommandChildren(L"Windows.CompressedFile.ExtractTo",&extractDestinations_,NamespaceMenuScope::Selection);
        const auto dc=GetDC(window_);
        const auto oldFont=dc&&font_?SelectObject(dc,font_):nullptr;
        if(extractDestinations_) for(UINT index=0;index<extractDestinations_->entries().size();++index) {
            const auto& entry=extractDestinations_->entries()[index];
            auto label=entry.label;
            // Explorer's three destination columns keep a fixed compact label
            // width; the complete native title/path remains in the tooltip.
            SIZE size{};
            if(dc&&GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&size)&&size.cx>px(60)) {
                size_t low=0,high=label.size();
                while(low<high) {
                    const auto middle=(low+high+1)/2;
                    const auto candidate=label.substr(0,middle)+L"\u2026";
                    if(GetTextExtentPoint32W(dc,candidate.c_str(),static_cast<int>(candidate.size()),&size)&&size.cx<=px(60))low=middle;
                    else high=middle-1;
                }
                if(low&&label[low-1]>=0xD800&&label[low-1]<=0xDBFF)--low;
                label.resize(low);label+=L"\u2026";
            }
            result.push_back({index,std::move(label),false,entry.icon,entry.description.empty()?entry.label:entry.description});
        }
        if(oldFont)SelectObject(dc,oldFont);
        if(dc)ReleaseDC(window_,dc);
    } else if (command == RibbonFrequentPlaces) {
        if(GetTickCount64()-frequentPlacesReadAt_>5000)refreshFrequentPlaces();
        displayedFrequentPlaces_=frequentPlaces_;
        for(UINT index=0;index<displayedFrequentPlaces_.size();++index) {
            const auto& place=displayedFrequentPlaces_[index];
            result.push_back({index,place.label,place.pinned,{},place.description});
        }
    } else if (command == RecentSearches) {
        for (UINT i = 0; i < recentSearches_.size(); ++i) result.push_back({i,recentSearches_[i],false});
    } else {
        const auto* choices = command == SearchKindMenu ? &kindChoices() : command == SearchDateMenu ? &dateChoices() :
            command == SearchSizeMenu ? &sizeChoices() : nullptr;
        if (choices) for (UINT i = 0; i < choices->size(); ++i) result.push_back({i,(*choices)[i].label,false});
        else if (command == RibbonSearchOtherProperties) {
            const auto& properties=otherPropertyChoices();
            for(UINT index=0;index<properties.size();++index)result.push_back({index,properties[index].label,false});
        }
        else if(ribbon_.layout()==RibbonLayout::InstalledWindows10) {
            updateNamespaceImpl();
            const auto binding=appCommandBinding(command);
            const auto key=binding&&!binding->commandStore.empty()?binding->commandStore:ribbonCommandStoreName(command);
            if(!key.empty()&&ribbonState(command).enabled) {
                auto& snapshot=ribbonCommandChildren_[command];
                if(snapshot&&snapshot->entries().empty())snapshot.reset();
                if(!snapshot) {
                    const auto scope=binding?binding->scope:NamespaceMenuScope::Selection;
                    auto& actions=scope==NamespaceMenuScope::Background?backgroundActions_:namespaceActions_;
                    actions.queryCommandChildren(key,&snapshot,scope);
                }
                if(snapshot) {
                    auto& paths=ribbonCommandPaths_[command];paths.clear();
                    const auto convert=[&](auto&& self,const std::vector<NamespaceSubcommandMetadata>& entries,
                                           std::vector<size_t> parent)->std::vector<RibbonItem> {
                        std::vector<RibbonItem> items;
                        UINT category=0;
                        for(size_t index=0;index<entries.size();++index) {
                            const auto& entry=entries[index];
                            if(entry.flags&ECF_ISSEPARATOR) {if(!items.empty())++category;continue;}
                            if(entry.state&ECS_HIDDEN)continue;
                            if((entry.flags&ECF_SEPARATORBEFORE)&&!items.empty())++category;
                            auto path=parent;path.push_back(index);
                            RibbonItem item{static_cast<UINT>(index),entry.label,false,entry.icon,entry.description,
                                SUCCEEDED(entry.stateStatus)&&!(entry.state&ECS_DISABLED),(entry.state&ECS_CHECKED)!=0};
                            item.invocationIndex=static_cast<UINT>(paths.size());paths.push_back(path);
                            item.children=self(self,entry.children,path);
                            item.checkable=(entry.flags&ECF_TOGGLEABLE)!=0;
                            item.category=category;
                            items.push_back(std::move(item));
                            if(entry.flags&ECF_SEPARATORAFTER)++category;
                        }
                        if(!category)for(auto& item:items)item.category=UI_COLLECTION_INVALIDINDEX;
                        return items;
                    };
                    result=convert(convert,snapshot->entries(),{});
                }
            }
            if(result.empty()&&(command==RibbonOptionsMenu||command==FolderOptions)) {
                std::wstring label;ribbon_.commandLabel(FolderOptions,label);
                result.push_back({FolderOptions,std::move(label),false});
            }
        }
    }
    return result;
}

HRESULT ExplorerApp::executeRibbonItem(UINT command, UINT item) {
    if(closing_)return E_ABORT;
    if(commandRefreshActive_) {deferCommandRefresh();return HRESULT_FROM_WIN32(ERROR_RETRY);}
    if(const auto found=ribbonCommandChildren_.find(command);found!=ribbonCommandChildren_.end()&&found->second) {
        if(namespaceDirty_)return HRESULT_FROM_WIN32(ERROR_RETRY);
        const auto paths=ribbonCommandPaths_.find(command);
        if(paths==ribbonCommandPaths_.end()||item>=paths->second.size())return E_INVALIDARG;
        auto path=paths->second[item];
        const auto generation=namespaceGeneration_;
        auto snapshot=std::move(found->second);
        const auto* entries=&snapshot->entries();
        for(const auto index:path) {
            if(index>=entries->size())return E_INVALIDARG;
            const auto& entry=(*entries)[index];
            if(FAILED(entry.stateStatus)||entry.state&(ECS_DISABLED|ECS_HIDDEN)||entry.flags&ECF_ISSEPARATOR)return E_ACCESSDENIED;
            entries=&entry.children;
        }
        if(!entries->empty()) {
            // Runtime Ribbon command galleries accept action/Boolean rows.
            // The selected provider's cascade remains a native Win32 menu;
            // leaf IDs retain the exact original command path and object.
            if(headless_||!window_||!IsWindowVisible(window_))return E_ACCESSDENIED;
            std::vector<std::vector<size_t>> leafPaths;
            std::vector<ComPtr<IUIImage>> images;
            const auto build=[&](auto&& self,const std::vector<NamespaceSubcommandMetadata>& children,
                                 std::vector<size_t> parent)->HMENU {
                const auto menu=CreatePopupMenu();if(!menu)return nullptr;
                for(size_t index=0;index<children.size();++index) {
                    const auto& child=children[index];if(child.state&ECS_HIDDEN)continue;
                    if(child.flags&ECF_ISSEPARATOR) {AppendMenuW(menu,MF_SEPARATOR,0,nullptr);continue;}
                    if(child.flags&ECF_SEPARATORBEFORE)AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                    auto childPath=parent;childPath.push_back(index);
                    MENUITEMINFOW info{sizeof(info)};info.fMask=MIIM_STRING|MIIM_STATE;
                    info.dwTypeData=const_cast<wchar_t*>(child.label.c_str());
                    info.fState=(FAILED(child.stateStatus)||child.state&ECS_DISABLED?MFS_DISABLED:MFS_ENABLED)|
                        (child.state&ECS_CHECKED?MFS_CHECKED:MFS_UNCHECKED);
                    if(!child.children.empty()) {
                        info.hSubMenu=self(self,child.children,childPath);
                        if(!info.hSubMenu){DestroyMenu(menu);return nullptr;}
                        info.fMask|=MIIM_SUBMENU;
                    } else {info.fMask|=MIIM_ID;info.wID=40000+static_cast<UINT>(leafPaths.size());leafPaths.push_back(std::move(childPath));}
                    ComPtr<IUIImage> image;HBITMAP bitmap=nullptr;
                    if(!child.icon.empty()&&SUCCEEDED(ribbon_.itemImage(child.icon,false,&image))&&SUCCEEDED(image->GetBitmap(&bitmap))) {
                        info.fMask|=MIIM_BITMAP;info.hbmpItem=bitmap;images.push_back(std::move(image));
                    }
                    if(!InsertMenuItemW(menu,GetMenuItemCount(menu),TRUE,&info)) {if(info.hSubMenu)DestroyMenu(info.hSubMenu);DestroyMenu(menu);return nullptr;}
                    if(child.flags&ECF_SEPARATORAFTER)AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                }
                return menu;
            };
            const auto menu=build(build,*entries,path);if(!menu)return HRESULT_FROM_WIN32(GetLastError()?GetLastError():ERROR_OUTOFMEMORY);
            POINT point{};GetCursorPos(&point);
            const auto selected=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window_,nullptr);
            DestroyMenu(menu);
            if(!selected)return S_FALSE;
            if(closing_||navigating_||namespaceGeneration_!=generation||namespaceDirty_)return HRESULT_FROM_WIN32(ERROR_RETRY);
            if(selected<40000||selected-40000>=leafPaths.size())return E_UNEXPECTED;
            path=std::move(leafPaths[selected-40000]);
        }
        const auto hr=snapshot->invokePath(path,headless_);
        if(SUCCEEDED(hr)){namespaceDirty_=selectionStateDirty_=true;scheduleDeferredUpdate();}
        return hr;
    }
    if((command==RibbonOptionsMenu||command==FolderOptions)&&item==0)return execute(FolderOptions);
    if(command==RibbonNewMenu) {
        if(namespaceDirty_)return HRESULT_FROM_WIN32(ERROR_RETRY);
        CommandRefreshScope nativeCommand(*this);
        return newItemTypes_?newItemTypes_->invoke(item,headless_):E_UNEXPECTED;
    }
    if(command==RibbonExtractToGallery) {
        if(namespaceDirty_)return HRESULT_FROM_WIN32(ERROR_RETRY);
        CommandRefreshScope nativeCommand(*this);
        return extractDestinations_?extractDestinations_->invoke(item,headless_):E_UNEXPECTED;
    }
    if (command == RibbonFrequentPlaces) {
        return item<displayedFrequentPlaces_.size()?browser_->BrowseToObject(displayedFrequentPlaces_[item].item.Get(),SBSP_ABSOLUTE):E_INVALIDARG;
    }
    if (command == RecentSearches) {
        if (item >= recentSearches_.size()) return E_INVALIDARG;
        return startSearch(recentSearches_[item],searchRecursive_);
    }
    if (command == RibbonSearchOtherProperties) {
        const auto& properties=otherPropertyChoices();
        if (item >= properties.size()) return E_INVALIDARG;
        const auto query = activeQuery_ + (activeQuery_.empty() ? L"" : L" ") + properties[item].expression;
        setSearchText(query); SetFocus(search_);
        SendMessageW(search_,EM_SETSEL,query.size(),query.size()); return S_OK;
    }
    const auto* choices = command == SearchKindMenu ? &kindChoices() : command == SearchDateMenu ? &dateChoices() :
        command == SearchSizeMenu ? &sizeChoices() : nullptr;
    if (!choices || item >= choices->size()) return E_INVALIDARG;
    const size_t category = command == SearchKindMenu ? 0 : command == SearchDateMenu ? 1 : 2;
    return startSearch(activeQuery_,searchRecursive_,category,(*choices)[item].expression);
}

HRESULT ExplorerApp::executeRibbon(UINT command) {
    if(closing_)return E_ABORT;
    if(commandRefreshActive_) {deferCommandRefresh();return HRESULT_FROM_WIN32(ERROR_RETRY);}
    if (command >= RibbonHomeTab && command <= RibbonDiscImageTab) return S_OK;
    if(command==NewFolder&&librariesRoot_)return newLibrary();
    HRESULT hr = S_OK;
    const auto binding = appCommandBinding(command);
    if (command == RibbonOpenWith || command == SortMenu || command == GroupMenu || command == ColumnsMenu || command == LibraryDefault ||
        command == LibraryOptimize || command == RibbonLibraryOptimizeMenu) {
        popup(command); return headless_ ? E_ACCESSDENIED : S_OK;
    }
    if (binding && binding->route != AppCommandRoute::Host) {
        updateNamespace();
        CommandRefreshScope nativeCommand(*this);
        auto& actions = command == Extract && archiveTargetValid_ ? archiveActions_ :
            binding->scope==NamespaceMenuScope::Background?backgroundActions_:namespaceActions_;
        activeNamespaceMenu_ = &actions;
        hr = invokeAppNativeCommand(actions, command, commandContext(), headless_);
        activeNamespaceMenu_ = nullptr;
        namespaceDirty_ = selectionStateDirty_ = true;
        if (SUCCEEDED(hr) && (command == RibbonResetLibrary || command == RibbonLibraryChangeIcon)) reloadLibrary();
    } else if ((command >= RibbonCopyToDesktop && command <= RibbonCopyToDownloads) ||
               (command >= RibbonMoveToDesktop && command <= RibbonMoveToDownloads)) {
        if (headless_) return E_ACCESSDENIED;
        const bool move = command >= RibbonMoveToDesktop;
        const UINT index = command - (move ? RibbonMoveToDesktop : RibbonCopyToDesktop);
        constexpr const KNOWNFOLDERID* folders[]{&FOLDERID_Desktop,&FOLDERID_Documents,&FOLDERID_Downloads};
        ComPtr<IShellItemArray> items; ComPtr<IShellItem> destination;
        hr = selection(items);
        if (SUCCEEDED(hr)) hr = SHGetKnownFolderItem(*folders[index],KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&destination));
        if (SUCCEEDED(hr)) hr = ShellOperations::copyOrMove(window_,items.Get(),destination.Get(),move);
        namespaceDirty_ = true;
    } else {
        switch (command) {
        case RibbonClearSearchHistory:
            recentSearches_.clear();
            if(searchSuggestions_)hr=searchSuggestions_->replace(recentSearches_);
            if(SUCCEEDED(hr)&&!headless_&&searchSuggestionsAllowed_)
                hr=saveSearchHistory(searchHistoryPath(),recentSearches_);
            ribbon_.invalidate(RecentSearches);ribbon_.invalidate(RibbonClearSearchHistory);
            break;
        case RibbonHelpButton: case RibbonHelp:
            if (headless_) return E_ACCESSDENIED;
            { SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.hwnd=window_; launch.lpVerb=L"open";
              launch.lpFile=L"https://support.microsoft.com/windows"; launch.nShow=SW_SHOWNORMAL;
              hr=ShellExecuteExW(&launch)?S_OK:HRESULT_FROM_WIN32(GetLastError()); } break;
        case RibbonAbout:
            if (headless_) return E_ACCESSDENIED;
            hr = ShellAboutW(window_,L"Windows Explorer",L"Native C++ Windows 10 Explorer",folderIcon_) ? S_OK : E_FAIL; break;
        case RibbonOpenSettings:
            if (headless_) return E_ACCESSDENIED;
            hr = reinterpret_cast<INT_PTR>(ShellExecuteW(window_,L"open",L"ms-settings:",nullptr,nullptr,SW_SHOWNORMAL)) > 32 ? S_OK : E_FAIL; break;
        case RibbonFolderOptions: hr = execute(FolderOptions); break;
        case RibbonNewProcess: hr = execute(NewWindow); break;
        case RibbonNewLibraryMenu: hr = execute(NewLibrary); break;
        case RibbonSearchThisPC: {
            if (!searchActive_) return S_FALSE;
            ComPtr<IShellItem> computer;
            hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&computer));
            if (FAILED(hr)) break;
            PIDLIST_ABSOLUTE raw=nullptr;
            hr=SHGetIDListFromObject(computer.Get(),&raw);
            if (FAILED(hr)) break;
            Pidl previous(searchScope_?ILCloneFull(searchScope_.get()):nullptr);
            auto previousScopes=searchScopes_;
            auto previousRules=searchScopeRules_;
            searchScope_.reset(raw);
            searchScopes_.Reset();
            searchScopeRules_.clear();
            hr=startSearch(activeQuery_,true);
            if (FAILED(hr)) {searchScope_=std::move(previous);searchScopes_=std::move(previousScopes);searchScopeRules_=std::move(previousRules);}
            break;
        }
        case RibbonExpandToCurrent: { const bool previous=expandCurrent_; expandCurrent_=!previous; hr=applyNavigationOptions(); if (FAILED(hr)) expandCurrent_=previous; break; }
        case RibbonShowAllFolders: { const bool previous=showAllFolders_; showAllFolders_=!previous; hr=applyNavigationOptions(); if (FAILED(hr)) showAllFolders_=previous; break; }
        case RibbonShowLibraries: { const bool previous=showLibraries_; showLibraries_=!previous; hr=applyNavigationOptions(); if (FAILED(hr)) showLibraries_=previous; break; }
        case RibbonLibraryShowInNavigation:
            if (headless_) return E_ACCESSDENIED;
            if (!library_.valid() || !library_.writable()) return E_ACCESSDENIED;
            { LIBRARYOPTIONFLAGS flags{}; hr = library_.native()->GetOptions(&flags);
              if (SUCCEEDED(hr)) hr = library_.native()->SetOptions(LOF_PINNEDTONAVPANE,
                  (flags & LOF_PINNEDTONAVPANE) ? LOF_DEFAULT : LOF_PINNEDTONAVPANE);
              if (SUCCEEDED(hr)) hr = commitLibrary(); } break;
        case LibraryLocations:
            if (headless_) return E_ACCESSDENIED;
            { ComPtr<IShellItem> item; hr=currentFolder(item);
              if (SUCCEEDED(hr)) hr=SHShowManageLibraryUI(item.Get(),window_,nullptr,nullptr,LMD_DEFAULT);
              if (SUCCEEDED(hr)) { reloadLibrary(); namespaceDirty_=true; } } break;
        case NewShortcut: case NewText: {
            if (headless_) return E_ACCESSDENIED;
            ComPtr<IShellItem> folder; NativeContextMenu menu;
            hr=currentFolder(folder);
            if (SUCCEEDED(hr)) hr=menu.createNewItems(window_,folder.Get(),view_.Get());
            std::vector<ContextMenuEntry> entries;
            if (SUCCEEDED(hr)) hr=menu.enumerate(entries);
            if (SUCCEEDED(hr)) {
                const wchar_t* verb=command==NewShortcut?L"NewLink":L".txt";
                const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry.canonicalVerb==verb && entry.enabled();});
                if (found==entries.end()) hr=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                else hr=menu.invoke(found->id);
            }
            break;
        }
        default: hr=execute(command); break;
        }
    }
    updateCommands(); updateStatus();
    return hr;
}

int ExplorerApp::headlessVisual(const PrivateDesktop& desktop, const std::filesystem::path& screenshot,
                              const std::filesystem::path& report, const VisualScene& scene) {
    auto failed = [&](int exitCode, const char* stage, HRESULT result = E_FAIL) {
        std::ofstream diagnostic(report);
        diagnostic << "{\"headless\":true,\"failedStage\":\"" << stage << "\",\"hresult\":" << static_cast<long>(result) << "}";
        return exitCode;
    };
    if (!headless_ || !desktop.ready() || FAILED(desktop.verifyIsolation()) || !screenshot.is_absolute() ||
        !report.is_absolute() || scene.width<300 || scene.height<200) return failed(5,"isolation");
    if (!pumpVisual([&]{return currentPidl_ && folderView_ && !navigating_;},15000)) return 6;
    dpi_=scene.dpi;
    if (scene.details != preferences_.detailsPane) {
        preferences_.detailsPane=scene.details; preferences_.previewPane=false;
        if (FAILED(recreateBrowser()) || !pumpVisual([&]{return folderView_ && !navigating_;},10000)) return 6;
    }
    const auto viewResult = scene.nativeView?S_OK:setView(scene.view);
    if (FAILED(viewResult)) return failed(7,"view",viewResult);
    if (!scene.select.empty()) {
        ComPtr<IShellItem> folder; currentFolder(folder);
        const auto folderPath=nameOf(folder.Get(),SIGDN_FILESYSPATH);
        const auto parsing=scene.select.starts_with(L"shell:") || scene.select.starts_with(L"::{") ||
            std::filesystem::path(scene.select).is_absolute() ? scene.select :
            (std::filesystem::path(folderPath)/scene.select).wstring();
        PIDLIST_ABSOLUTE raw=nullptr;
        auto hr=SHParseDisplayName(parsing.c_str(),nullptr,&raw,0,nullptr);
        Pidl selected(raw);
        if (FAILED(hr) || !selected) return failed(7,"selection-parse",hr);
        const auto chosen = pumpVisual([&]{
            hr=view_->SelectItem(ILFindLastID(selected.get()),SVSI_SELECT|SVSI_DESELECTOTHERS);
            if (FAILED(hr)) return false;
            ComPtr<IShellItemArray> current; DWORD count=0;
            return SUCCEEDED(selection(current)) && current && SUCCEEDED(current->GetCount(&count)) && count==1;
        },5000);
        if (!chosen) return failed(7,"selection",hr);
        selectionStateDirty_=namespaceDirty_=true; updateCommands();
    }
    const std::map<std::wstring,UINT> pages{
        {L"Home",RibbonHomeTab},{L"Share",RibbonShareTab},{L"View",RibbonViewTab},{L"Computer",RibbonComputerTab},
        {L"Picture",RibbonPictureTab},{L"Drive",RibbonDriveTab},{L"Compressed",RibbonCompressedTab},
        {L"Search",RibbonSearchTab},{L"Library",RibbonLibraryTab},{L"Recycle",RibbonRecycleTab},
        {L"Application",RibbonApplicationTab},{L"Music",RibbonMusicTab},{L"Video",RibbonVideoTab},
        {L"DiskImage",RibbonDiscImageTab},{L"DiscImage",RibbonDiscImageTab},
        {L"Network",RibbonNetworkTab},{L"Shortcut",RibbonShortcutTab}};
    const auto found=pages.find(scene.page);
    if (found==pages.end()) return 2;
    visualPage_=found->second;
    if (found->second==RibbonHomeTab || found->second==RibbonShareTab) ribbon_.setComputerMode(false);
    if (found->second==RibbonComputerTab) ribbon_.setComputerMode(true);
    if(found->second==RibbonNetworkTab) {
        const auto directory=commandCapabilities_.find(RibbonSearchActiveDirectory);
        ribbon_.setNetworkMode(true,directory!=commandCapabilities_.end()&&directory->second.enabled);
    }
    // Context scenes normally come from actual selected items/library/search.
    // An explicit page may expose its native layout for an isolated empty scene.
    if (found->second>=RibbonPictureTab && found->second<=RibbonDiscImageTab) {
        constexpr RibbonContext contexts[]{RibbonContext::Picture,RibbonContext::Drive,RibbonContext::Compressed,
            RibbonContext::Search,RibbonContext::Library,RibbonContext::Recycle,RibbonContext::Application,
            RibbonContext::Music,RibbonContext::Video,RibbonContext::DiscImage};
        ribbon_.setContexts(ribbonContexts_|contexts[found->second-RibbonPictureTab],true);
    }
    if(found->second==RibbonShortcutTab)ribbon_.setContexts(ribbonContexts_|RibbonContext::Shortcut,true);
    ribbon_.setMinimized(scene.collapsed);
    if (!SetWindowPos(window_,nullptr,0,0,scene.width,scene.height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)) return failed(7,"geometry",HRESULT_FROM_WIN32(GetLastError()));
    if (FAILED(desktop.verifyIsolation())) return 5;
    ShowWindow(window_,SW_SHOWNOACTIVATE); SetActiveWindow(window_); UpdateWindow(window_);
    HRESULT tabResult=E_PENDING;
    const auto tabReady=pumpVisual([&]{tabResult=ribbon_.selectTab(found->second);return SUCCEEDED(tabResult);},1000);
    if (!tabReady) {
        VisualCaptureOptions diagnosticOptions; diagnosticOptions.trimInvisibleFrame=true;
        VisualCaptureReport diagnosticCapture;
        captureWindowPng(desktop,window_,screenshot,diagnosticOptions,diagnosticCapture);
        writeVisualCaptureReport(report,diagnosticCapture);
        return 7;
    }
    updateCommands(); layout(); ribbon_.flush();
    const auto settle=GetTickCount64()+500;
    pumpVisual([&]{return GetTickCount64()>=settle;},1000);
    if (FAILED(desktop.verifyIsolation())) return 5;
    SetActiveWindow(window_);applyWindowTheme(window_);layout();
    if(GetActiveWindow()!=window_)return failed(7,"private-active-frame",E_FAIL);
    // Theme/layout invalidation must finish through the real window paint
    // procedures. On Windows 10 PrintWindow can copy an existing surface
    // without sending WM_PRINT; an unpainted owner-drawn child stays blank.
    if(!RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|
        RDW_ALLCHILDREN|RDW_UPDATENOW))return failed(7,"native-repaint",HRESULT_FROM_WIN32(GetLastError()));
    const auto painted=pumpVisual([&] {
        return !GetUpdateRect(window_,nullptr,FALSE) &&
            (!ribbonCollapse_ || !GetUpdateRect(ribbonCollapse_,nullptr,FALSE));
    },1000);
    if(!painted || !GdiFlush())return failed(7,"native-paint-settle",E_FAIL);
    VisualCaptureOptions options;
    options.trimInvisibleFrame=true; options.layoutDpi=scene.dpi;
    options.ribbonFramework=ribbon_.framework();
    options.ribbonLayout=ribbon_.layout()==RibbonLayout::InstalledWindows10?L"InstalledWindows10":L"Authored";
    options.installedRibbonStatus=ribbon_.installedLayoutStatus();
    options.ribbonFeaturesRead=true;
    options.ribbonFeatures=ribbon_.features();
    if(found->second==RibbonShareTab) {
        constexpr std::pair<UINT,std::wstring_view> commands[]{
            {RibbonEmail,L"Windows.email"},{RibbonSpecificPeople,L"Windows.ShareSpecificUsers"},{RibbonStopSharing,L"Windows.SharePrivate"}};
        for(const auto& [command,key]:commands) {
            VisualCaptureOptions::ProviderReadback provider;
            provider.command=command;provider.selectedCount=selectionCount_;
            const auto cached=commandCapabilities_.find(command);
            if(cached!=commandCapabilities_.end()) {provider.cachedRead=cached->second.status;provider.cachedEnabled=cached->second.enabled;}
            else provider.cachedRead=HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            NamespaceCommandState actual;
            provider.nativeRead=namespaceActions_.queryCommandState(key,&actual,NamespaceMenuScope::Selection);
            if(SUCCEEDED(provider.nativeRead))provider.nativeState=actual.state;
            options.ribbonProviders.push_back(provider);
        }
    }
    for(UINT bit=0;bit<11;++bit) {
        VisualCaptureOptions::ContextReadback context;
        context.logicalContext=1u<<bit;
        context.read=ribbon_.contextAvailable(static_cast<RibbonContext>(context.logicalContext),
            context.nativeIdentifier,context.availability);
        options.ribbonContexts.push_back(context);
    }
    VisualCaptureReport captured;
    const auto hr=captureWindowPng(desktop,window_,screenshot,options,captured);
    ShowWindow(window_,SW_HIDE);
    const auto reported=writeVisualCaptureReport(report,captured);
    return SUCCEEDED(hr)&&SUCCEEDED(reported)?0:8;
}
}
