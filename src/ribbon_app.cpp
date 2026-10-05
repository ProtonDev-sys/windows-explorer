#include "explorer/app.hpp"
#include "explorer/ribbon_commands.hpp"
#include "explorer/saved_search.hpp"
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
#include <cstring>
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
        std::vector<std::wstring> values;
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
                    result.push_back({title,{}}); values.emplace_back(value.pwszVal);
                }
                CoTaskMemFree(title); PropVariantClear(&value);
            }
        }
        std::vector<std::wstring> expressions;
        if (FAILED(NativeSearchRefinements::kindPresets(values, &expressions)) || expressions.size() != result.size()) return std::vector<SearchChoice>{};
        for (size_t index = 0; index < result.size(); ++index) result[index].expression = std::move(expressions[index]);
        return result;
    }();
    return choices;
}

const std::vector<SearchChoice>& dateChoices() {
    static thread_local const std::vector<SearchChoice> choices = [] {
        std::vector<SearchChoice> result{
        {L"Today",L"System.DateModified:System.StructuredQueryType.DateTime#Today"},
        {L"Yesterday",L"System.DateModified:System.StructuredQueryType.DateTime#Yesterday"},
        {L"This week",L"System.DateModified:System.StructuredQueryType.DateTime#ThisWeek"},
        {L"Last week",L"System.DateModified:System.StructuredQueryType.DateTime#LastWeek"},
        {L"This month",L"System.DateModified:System.StructuredQueryType.DateTime#ThisMonth"},
        {L"Last month",L"System.DateModified:System.StructuredQueryType.DateTime#LastMonth"},
        {L"This year",L"System.DateModified:System.StructuredQueryType.DateTime#ThisYear"},
        {L"Last year",L"System.DateModified:System.StructuredQueryType.DateTime#LastYear"}};
        // The installed Date provider supplies the localized eight-preset
        // order. Accept its complete shape before pairing those titles with
        // the language-independent query tokens; a partial/cascade provider
        // cannot silently shift the meaning of a displayed choice.
        std::vector<NamespaceSubcommandMetadata> native;
        if (SUCCEEDED(namespaceCommandChildren(L"Windows.SearchFilterDate", nullptr, nullptr, &native)) &&
            native.size() == result.size() && std::all_of(native.begin(), native.end(), [](const auto& entry) {
                return !entry.label.empty() && entry.flags == ECF_DEFAULT && entry.children.empty();
            })) {
            for (size_t index = 0; index < result.size(); ++index) result[index].label = native[index].label;
        }
        return result;
    }();
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
    struct Wake {HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Wake(){if(value)CloseHandle(value);}} wake;
    if(!wake.value)return false;
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        unsigned dispatched=0;
        while (dispatched++<16&&GetTickCount64()<end&&PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (ready()) return true;
        const auto now=GetTickCount64();if(now>=end)break;
        DWORD signaled=0;
        const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,
            static_cast<DWORD>(std::min<ULONGLONG>(5,end-now)),1,&wake.value,&signaled);
        if(FAILED(waited)&&waited!=RPC_S_CALLPENDING)return false;
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

HRESULT ExplorerApp::applyNavigationOptions(bool expandOnce) {
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
    // View creation posts another options pass after navigation completion.
    // Retain an explicit one-time expansion on this exact native tree and
    // navigation generation even though the persistent toggle is disabled.
    if(!expandCurrent_&&!expandOnce&&navigationExpansionOneTime_&&navigationTree_&&
       !navigationExpansion_.empty()&&!navigating_&&navigationExpansionGeneration_==navigationCount_) {
        ComPtr<IUnknown> previousIdentity,currentIdentity;
        if(SUCCEEDED(navigationTree_.As(&previousIdentity))&&SUCCEEDED(tree.As(&currentIdentity))&&
           previousIdentity.Get()==currentIdentity.Get())return hr;
    }
    KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();navigationExpansionOneTime_=false;
    if(expandCurrent_||expandOnce) {
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
        navigationExpansionOneTime_=expandOnce;
        navigationExpansionDeadline_=GetTickCount64()+5000;
        SetTimer(window_,3,15,nullptr);advanceNavigationExpansion();
    }
    return hr;
}

void ExplorerApp::advanceNavigationExpansion() {
    if(!navigationTree_||navigationExpansion_.empty()||closing_||navigating_||
        navigationExpansionGeneration_!=navigationCount_||GetTickCount64()>=navigationExpansionDeadline_) {
        KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();navigationExpansionOneTime_=false;return;
    }
    auto& item=navigationExpansion_[navigationExpansionIndex_];
    // A hidden native root and an off-screen child can be materialized without
    // a drawable rectangle. Item state, rather than geometry, proves presence.
    NSTCITEMSTATE state{};
    if(FAILED(navigationTree_->GetItemState(item.Get(),NSTCIS_EXPANDED,&state)))return;
    if(navigationExpansionIndex_+1==navigationExpansion_.size()) {
        navigationTree_->EnsureItemVisible(item.Get());
        KillTimer(window_,3);navigationExpansion_.clear();navigationTree_.Reset();navigationExpansionOneTime_=false;return;
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
    headlessCompletedPendingCommand_.reset();
    headlessCompletedPendingOriginalStatus_=E_PENDING;
    if(selectionStateBatch_)selectionStateBatch_->cancel();
    selectionStateBatch_.reset();selectionStateBindings_.clear();
    for(auto& [id,task]:commandStateTasks_) { (void)id;task->cancel(); }
    commandStateTasks_.clear();
    // Fast state remains immediately available. Give selection/navigation
    // notifications one short settling interval before starting slow native
    // menu providers: cancellation cannot interrupt an in-flight Shell call.
    commandStateStartAt_=GetTickCount64()+100;
}

void ExplorerApp::startPendingCommandStates() {
    if(closing_)return;
    const auto now=GetTickCount64();
    if(now<commandStateStartAt_) {
        SetTimer(window_,4,static_cast<UINT>(commandStateStartAt_-now),nullptr);
        return;
    }
    if(!selectionStateBatch_) {
        std::vector<AppCommandCapability> batch;
        for(const auto& [id,capability]:commandCapabilities_)
            if(capability.status==E_PENDING&&!capability.slowStateCompleted&&!capability.selectionVerbs.empty()&&
               capability.binding.scope==NamespaceMenuScope::Selection&&!(id==Extract&&archiveTargetValid_)&&
               !commandStateTasks_.contains(id))batch.push_back(capability);
        if(!batch.empty()) {
            const auto status=startAppSelectionStateBatch(namespaceActions_,batch,&selectionStateBatch_,&selectionStateBindings_);
            if(FAILED(status)&&status!=HRESULT_FROM_WIN32(ERROR_BUSY))
                for(const auto& capability:batch)completeCommandState(capability.binding.command,status,nullptr);
        }
    }
    for(auto& [id,capability]:commandCapabilities_) {
        if(capability.status!=E_PENDING||capability.slowStateCompleted)continue;
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
        else if(hr!=HRESULT_FROM_WIN32(ERROR_BUSY)) {
            capability.status=hr;capability.enabled=false;capability.checked=false;capability.slowStateCompleted=true;
        }
    }
    const bool pending=std::any_of(commandCapabilities_.begin(),commandCapabilities_.end(),
        [](const auto& entry){return entry.second.status==E_PENDING&&!entry.second.slowStateCompleted;});
    if(pending)SetTimer(window_,4,50,nullptr);else KillTimer(window_,4);
}

void ExplorerApp::completeCommandState(UINT command,HRESULT status,const NamespaceCommandState* native) {
    const auto found=commandCapabilities_.find(command);
    if(found==commandCapabilities_.end())return;
    auto& capability=found->second;capability.status=status;capability.slowStateCompleted=true;
    if(SUCCEEDED(status)&&native)capability.native=*native;
    capability.enabled=SUCCEEDED(status)&&native&&native->enabled();
    capability.checked=SUCCEEDED(status)&&native&&native->checked();
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
    // Selection notifications may repeat for the same full native selection.
    // Read it back before deciding whether the retained provider work is stale.
    // updateNamespaceImpl cancels that work when its actual target changes.
    if(selectionStateDirty_||namespaceDirty_) {updateCommands();return;}
    CommandRefreshScope scope(*this);
    if(selectionStateBatch_) {
        std::vector<NamespaceSelectionVerbState> states;
        // Snapshot completion BEFORE polling: a worker can finish between the
        // calls. A pending poll with finished=false is checked on the next tick.
        const bool finished=selectionStateBatch_->completed();
        auto status=selectionStateBatch_->pollSelectionVerbBatch(&states);
        if(status!=E_PENDING||finished) {
            if(headless_) {
                HeadlessStateWorkerTiming timing;
                timing.selectionBatch=true;timing.status=status;
                timing.timingStatus=selectionStateBatch_->pollTimings(&timing.native);
                commandTimings_.completedStateWorkers.push_back(timing);
            }
            if(headless_&&headlessCompletedPendingCommand_&&
               std::any_of(selectionStateBindings_.begin(),selectionStateBindings_.end(),
                   [&](const auto& binding){return binding.command==*headlessCompletedPendingCommand_;})) {
                headlessCompletedPendingCommand_.reset();headlessCompletedPendingOriginalStatus_=status;status=E_PENDING;
            }
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
        const bool finished=task->second->completed();
        auto hr=task->second->poll(&native);
        if(hr==E_PENDING&&!finished) {++task;continue;}
        if(headless_) {
            HeadlessStateWorkerTiming timing;
            timing.command=task->first;timing.status=hr;
            timing.timingStatus=task->second->pollTimings(&timing.native);
            commandTimings_.completedStateWorkers.push_back(timing);
        }
        if(headless_&&headlessCompletedPendingCommand_==task->first) {
            headlessCompletedPendingCommand_.reset();headlessCompletedPendingOriginalStatus_=hr;hr=E_PENDING;
        }
        completeCommandState(task->first,hr,SUCCEEDED(hr)?&native:nullptr);
        task=commandStateTasks_.erase(task);
    }
    startPendingCommandStates();
}

void ExplorerApp::fitBreadcrumbs(int width) {
    if (!breadcrumbs_ || width <= 0 || breadcrumbLayoutActive_ || breadcrumbsPidls_.empty() ||
        breadcrumbLabels_.size() != breadcrumbsPidls_.size()) return;
    struct LayoutScope {
        bool& active;
        explicit LayoutScope(bool& value) : active(value) { active = true; }
        ~LayoutScope() { active = false; }
    } scope(breadcrumbLayoutActive_);
    const auto hot = SendMessageW(breadcrumbs_, TB_GETHOTITEM, 0, 0);
    TBBUTTON oldHot{};
    const bool hadHot = hot >= 0 && SendMessageW(breadcrumbs_, TB_GETBUTTON, hot, reinterpret_cast<LPARAM>(&oldHot));
    const auto hotAncestor = hadHot ? breadcrumbAncestor(static_cast<UINT>(oldHot.idCommand)) : std::optional<size_t>{};
    width = std::max(px(18), width - px(11));
    const auto dc=GetDC(breadcrumbs_);
    const auto nativeFont=reinterpret_cast<HFONT>(SendMessageW(breadcrumbs_,WM_GETFONT,0,0));
    const auto selectedFont=nativeFont?nativeFont:font_;
    const auto oldFont=dc&&selectedFont?SelectObject(dc,selectedFont):nullptr;
    std::vector<int> widths;
    widths.reserve(breadcrumbLabels_.size());
    for(size_t index=0;index<breadcrumbLabels_.size();++index) {
        const auto& label = breadcrumbLabels_[index];
        SIZE extent{};if(dc)GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&extent);
        widths.push_back(index==0&&breadcrumbsPidls_.size()>1?px(34):
            std::max(px(24),std::min(px(220),static_cast<int>(extent.cx)+px(index==0?42:24))));
    }
    if(oldFont)SelectObject(dc,oldFont);if(dc)ReleaseDC(breadcrumbs_,dc);
    const int addressWidth = px(18), overflowWidth = px(20);
    long long used = addressWidth;
    for (const auto value : widths) used += value;
    size_t firstVisible = 1;
    const bool overflowReserved = widths.size() > 1 && used > width;
    if (overflowReserved) {
        used += overflowWidth;
        while (firstVisible + 1 < widths.size() && used > width) used -= widths[firstVisible++];
    }
    // IDs belong to rendered slots, never ancestor depth. Only native command
    // ID capacity can restrict rendered buttons; every earlier node remains
    // available through the overflow model instead of being discarded.
    constexpr size_t maximumButtons = 0xffff - BreadcrumbFirst + 1;
    while (widths.size() - firstVisible + 1 > maximumButtons) used -= widths[firstVisible++];
    bool overflow = firstVisible > 1;
    if (overflow && !overflowReserved) used += overflowWidth;
    if (!overflow && overflowReserved) used -= overflowWidth;
    const auto leaf = widths.size() - 1;
    const bool rootVisible = !leaf || width >= addressWidth + widths.front() + (overflow ? overflowWidth : 0) + px(8);
    if (!rootVisible) {
        used -= widths.front();
        if (!overflow) { overflow = true; used += overflowWidth; }
    }
    const int fixedWidth = addressWidth + (leaf && rootVisible ? widths.front() : 0) + (overflow ? overflowWidth : 0);
    const int leafWidth = std::min(widths[leaf], std::max(px(8), width - fixedWidth));
    used -= widths[leaf] - leafWidth; widths[leaf] = leafWidth;
    std::vector<size_t> visible, hidden;
    if (rootVisible) visible.push_back(0); else hidden.push_back(0);
    for (size_t index = 1; index < firstVisible; ++index) hidden.push_back(index);
    for (size_t index = firstVisible; index < widths.size(); ++index) visible.push_back(index);
    breadcrumbButtons_.clear(); breadcrumbHiddenAncestors_.clear();
    while (SendMessageW(breadcrumbs_, TB_BUTTONCOUNT, 0, 0)) SendMessageW(breadcrumbs_, TB_DELETEBUTTON, 0, 0);
    const auto addAncestor = [&](size_t slot) {
        const auto index = visible[slot];
        TBBUTTON button{}; button.iBitmap = index == 0 ? 0 : I_IMAGENONE;
        button.idCommand = BreadcrumbFirst + static_cast<int>(slot);
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_DROPDOWN | BTNS_SHOWTEXT;
        button.dwData = index == 0 && widths.size() > 1 ? 1 : 0;
        button.iString = reinterpret_cast<INT_PTR>(breadcrumbLabels_[index].c_str());
        return SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button)) != FALSE;
    };
    bool added = true;
    if (rootVisible) added = addAncestor(0);
    if (overflow) {
        TBBUTTON button{}; button.iBitmap = I_IMAGENONE; button.idCommand = BreadcrumbOverflow;
        button.fsState = TBSTATE_ENABLED; button.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
        button.iString = reinterpret_cast<INT_PTR>(L"Earlier locations");
        added = SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&button)) && added;
    }
    for (size_t slot = rootVisible ? 1 : 0; slot < visible.size(); ++slot) added = addAncestor(slot) && added;
    TBBUTTON spacer{}; spacer.idCommand = 0; spacer.fsStyle = BTNS_SEP;
    added = SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&spacer)) && added;
    TBBUTTON addresses{}; addresses.iBitmap = I_IMAGENONE; addresses.idCommand = AddressList;
    addresses.fsState = TBSTATE_ENABLED; addresses.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
    addresses.iString = reinterpret_cast<INT_PTR>(L"Recent locations");
    added = SendMessageW(breadcrumbs_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&addresses)) && added;
    if (!added) { showError(E_OUTOFMEMORY, L"Create breadcrumb buttons"); return; }
    // Native insertion/theme changes invalidate assigned widths. Set widths
    // only after the complete rendered collection is present.
    for (size_t slot = 0; slot < visible.size(); ++slot) {
        TBBUTTONINFOW size{sizeof(size)}; size.dwMask = TBIF_SIZE;
        size.cx = static_cast<WORD>(widths[visible[slot]]);
        SendMessageW(breadcrumbs_, TB_SETBUTTONINFOW, BreadcrumbFirst + slot, reinterpret_cast<LPARAM>(&size));
    }
    TBBUTTONINFOW size{sizeof(size)}; size.dwMask = TBIF_SIZE;
    if (overflow) { size.cx = static_cast<WORD>(overflowWidth); SendMessageW(breadcrumbs_, TB_SETBUTTONINFOW, BreadcrumbOverflow, reinterpret_cast<LPARAM>(&size)); }
    size.cx = static_cast<WORD>(std::clamp<long long>(width - used, 0, 65535));
    SendMessageW(breadcrumbs_, TB_SETBUTTONINFOW, 0, reinterpret_cast<LPARAM>(&size));
    SendMessageW(breadcrumbs_, TB_HIDEBUTTON, 0, MAKELONG(size.cx == 0, 0));
    size.cx = static_cast<WORD>(addressWidth);
    SendMessageW(breadcrumbs_, TB_SETBUTTONINFOW, AddressList, reinterpret_cast<LPARAM>(&size));
    breadcrumbButtons_ = std::move(visible); breadcrumbHiddenAncestors_ = std::move(hidden);
    if (hadHot) {
        UINT restored = static_cast<UINT>(oldHot.idCommand);
        if (hotAncestor) {
            const auto found = std::find(breadcrumbButtons_.begin(), breadcrumbButtons_.end(), *hotAncestor);
            restored = found == breadcrumbButtons_.end() ? static_cast<UINT>(BreadcrumbOverflow) :
                BreadcrumbFirst + static_cast<UINT>(found - breadcrumbButtons_.begin());
        } else if (restored == BreadcrumbOverflow && !overflow) restored = BreadcrumbFirst;
        const auto position = SendMessageW(breadcrumbs_, TB_COMMANDTOINDEX, restored, 0);
        if (position >= 0) SendMessageW(breadcrumbs_, TB_SETHOTITEM, position, 0);
    }
}

std::optional<size_t> ExplorerApp::breadcrumbAncestor(UINT command) const {
    if (command < BreadcrumbFirst || command - BreadcrumbFirst >= breadcrumbButtons_.size()) return {};
    const auto index = breadcrumbButtons_[command - BreadcrumbFirst];
    return index < breadcrumbsPidls_.size() ? std::optional<size_t>{index} : std::optional<size_t>{};
}

HRESULT ExplorerApp::breadcrumbDropdownTarget(UINT command, IShellItem** parent, IShellItem** selected) const {
    if (!parent || !selected) return E_POINTER;
    const auto index = breadcrumbAncestor(command);
    if (!index) return E_INVALIDARG;
    const auto generation = navigationCount_;
    Pidl parentId(ILCloneFull(breadcrumbsPidls_[*index].get()));
    Pidl selectedId(*index + 1 < breadcrumbsPidls_.size() ? ILCloneFull(breadcrumbsPidls_[*index + 1].get()) : nullptr);
    if (!parentId || (*index + 1 < breadcrumbsPidls_.size() && !selectedId)) return E_OUTOFMEMORY;
    ComPtr<IShellItem> nativeParent, nativeChild;
    auto hr = SHCreateItemFromIDList(parentId.get(), IID_PPV_ARGS(&nativeParent));
    if (SUCCEEDED(hr) && selectedId) hr = SHCreateItemFromIDList(selectedId.get(), IID_PPV_ARGS(&nativeChild));
    if (FAILED(hr)) return hr;
    if (closing_ || generation != navigationCount_) return E_ABORT;
    *parent = nativeParent.Detach(); *selected = nativeChild.Detach(); return S_OK;
}

HRESULT ExplorerApp::createBreadcrumbOverflowMenu(HMENU* result, std::vector<Pidl>* targets) const {
    if (!result || !targets) return E_POINTER;
    if (breadcrumbHiddenAncestors_.empty()) return S_FALSE;
    struct MenuOwner {
        HMENU value = CreatePopupMenu();
        ~MenuOwner() { if (value) DestroyMenu(value); }
    } menu;
    if (!menu.value) return HRESULT_FROM_WIN32(GetLastError());
    std::vector<Pidl> locations;
    locations.reserve(breadcrumbHiddenAncestors_.size());
    for (const auto index : breadcrumbHiddenAncestors_) {
        if (index >= breadcrumbsPidls_.size() || index >= breadcrumbLabels_.size()) return E_UNEXPECTED;
        Pidl location(ILCloneFull(breadcrumbsPidls_[index].get()));
        if (!location) return E_OUTOFMEMORY;
        std::wstring label;
        for (const auto character : breadcrumbLabels_[index]) { label += character; if (character == L'&') label += character; }
        locations.push_back(std::move(location));
        if (!AppendMenuW(menu.value, MF_STRING, locations.size(), label.c_str())) return HRESULT_FROM_WIN32(GetLastError());
    }
    *targets = std::move(locations); *result = menu.value; menu.value = nullptr; return S_OK;
}

HRESULT ExplorerApp::browseBreadcrumbTarget(PCIDLIST_ABSOLUTE target, unsigned generation) {
    if (!target) return E_INVALIDARG;
    if (closing_ || generation != navigationCount_) return E_ABORT;
    if (navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (!browser_) return E_UNEXPECTED;
    Pidl location(ILCloneFull(target));
    if (!location) return E_OUTOFMEMORY;
    const auto browser = browser_;
    return browser->BrowseToIDList(location.get(), SBSP_ABSOLUTE);
}

HRESULT ExplorerApp::browseBreadcrumb(UINT command) {
    const auto index = breadcrumbAncestor(command);
    return index ? browseBreadcrumbTarget(breadcrumbsPidls_[*index].get(), navigationCount_) : E_INVALIDARG;
}

HRESULT ExplorerApp::showBreadcrumbOverflow() {
    if (headless_) return E_ACCESSDENIED;
    if (closing_ || navigating_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    const auto generation = navigationCount_;
    HMENU menu = nullptr; std::vector<Pidl> targets;
    auto hr = createBreadcrumbOverflowMenu(&menu, &targets);
    if (hr != S_OK) return hr;
    RECT button{};
    if (!SendMessageW(breadcrumbs_, TB_GETRECT, BreadcrumbOverflow, reinterpret_cast<LPARAM>(&button))) {
        DestroyMenu(menu); return E_UNEXPECTED;
    }
    hr = mapUiRect(breadcrumbs_, nullptr, button, &button);
    UiPopupPlacement placement;
    if (SUCCEEDED(hr)) hr = popupUiPlacement(window_, button, TPM_RETURNCMD | TPM_NONOTIFY, &placement);
    if (FAILED(hr)) { DestroyMenu(menu); return hr; }
    const auto selected = TrackPopupMenu(menu, placement.flags,
        placement.anchor.x, placement.anchor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (!selected) return S_FALSE;
    if (selected > targets.size()) return E_UNEXPECTED;
    return browseBreadcrumbTarget(targets[selected - 1].get(), generation);
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
        const auto mapped = mapUiPoint(nullptr, breadcrumbs_, point, &point);
        if (FAILED(mapped)) return mapped;
        const auto index=SendMessageW(breadcrumbs_,TB_HITTEST,0,reinterpret_cast<LPARAM>(&point));
        TBBUTTON button{};
        if (index<0 || !SendMessageW(breadcrumbs_,TB_GETBUTTON,index,reinterpret_cast<LPARAM>(&button))) return S_FALSE;
        const auto ancestor = breadcrumbAncestor(static_cast<UINT>(button.idCommand));
        return ancestor ? SHCreateItemFromIDList(breadcrumbsPidls_[*ancestor].get(),IID_PPV_ARGS(item)) : S_FALSE;
    };
    ComPtr<BreadcrumbDropTarget> target;
    auto hr=BreadcrumbDropTarget::create(breadcrumbs_,options,&target);
    if (SUCCEEDED(hr)) hr=target->registerWindow();
    if (SUCCEEDED(hr)) breadcrumbDrop_=std::move(target);
}

void ExplorerApp::beginBreadcrumbMenu(UINT command) {
    if (headless_ || navigating_ || !breadcrumbAncestor(command)) return;
    if (breadcrumbTask_) { breadcrumbTask_->cancel(); breadcrumbTask_.reset(); }
    const auto generation = navigationCount_;
    ComPtr<IShellItem> parent,selected;
    auto hr=breadcrumbDropdownTarget(command,&parent,&selected);
    if (FAILED(hr)) { showError(hr,L"Read breadcrumb folders"); return; }
    if (navigating_ || closing_ || generation != navigationCount_) return;
    RECT button{};
    if (!SendMessageW(breadcrumbs_,TB_GETRECT,command,reinterpret_cast<LPARAM>(&button))) return;
    hr = mapUiRect(breadcrumbs_, nullptr, button, &button);
    UiPopupPlacement placement;
    if (SUCCEEDED(hr)) hr = popupUiPlacement(window_, button, TPM_RETURNCMD | TPM_NONOTIFY, &placement);
    if (FAILED(hr)) { showError(hr, L"Read breadcrumb folders"); return; }
    breadcrumbMenuPoint_=placement.anchor;
    breadcrumbGeneration_=generation;
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
        std::wstring label;
        for (const auto character : snapshot.children[i].label) { label += character; if (character == L'&') label += character; }
        AppendMenuW(menu,MF_STRING,first+i,label.c_str());
        if (snapshot.children[i].selected) SetMenuDefaultItem(menu,first+i,FALSE);
    }
    if (snapshot.children.empty()) AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"No subfolders");
    if (!snapshot.complete) AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"Additional folders are still unavailable");
    UINT menuFlags = 0;
    const auto placementRead = popupUiFlags(window_, TPM_RETURNCMD | TPM_NONOTIFY, &menuFlags);
    if (FAILED(placementRead)) { DestroyMenu(menu); showError(placementRead, L"Read breadcrumb folders"); return; }
    const auto selected=TrackPopupMenu(menu,menuFlags,
        breadcrumbMenuPoint_.x,breadcrumbMenuPoint_.y,0,window_,nullptr);
    DestroyMenu(menu);
    if (selected>=first && selected-first<snapshot.children.size() && !navigating_ && breadcrumbGeneration_==navigationCount_)
        showError(browser_->BrowseToObject(snapshot.children[selected-first].item.Get(),SBSP_ABSOLUTE),L"Open breadcrumb folder");
}

void ExplorerApp::initializeSearchRefinementChoices() {
    // Load/validate enum syntax before LoadUI can request a collection. Native
    // parser work belongs to the owner STA, outside Ribbon property callbacks.
    (void)kindChoices();
}

void ExplorerApp::refreshSearchRefinements() {
    if (!searchActive_) {
        searchRefinements_.reset(); searchRefinementQuery_.clear(); searchRefinementStatus_ = E_PENDING;
        searchRefinementInspected_ = false;
        searchRefinementSelected_.fill(UI_COLLECTION_INVALIDINDEX); return;
    }
    if (searchRefinementQuery_ == activeQuery_ && searchRefinementInspected_) return;
    const auto query = activeQuery_;
    const auto revision = searchInteractionRevision_;
    const auto navigation = navigationCount_;
    std::shared_ptr<NativeSearchRefinements> snapshot;
    auto hr = NativeSearchRefinements::inspect(query, &snapshot);
    std::array<UINT, 3> selected{UI_COLLECTION_INVALIDINDEX, UI_COLLECTION_INVALIDINDEX, UI_COLLECTION_INVALIDINDEX};
    const std::array<const std::vector<SearchChoice>*, 3> choices{&kindChoices(), &dateChoices(), &sizeChoices()};
    for (unsigned category = 0; SUCCEEDED(hr) && category < choices.size(); ++category) {
        for (UINT index = 0; index < choices[category]->size(); ++index) {
            bool same = false;
            hr = snapshot->matches(static_cast<SearchRefinementCategory>(category), (*choices[category])[index].expression, &same);
            if (FAILED(hr)) break;
            if (same) {
                if (selected[category] != UI_COLLECTION_INVALIDINDEX) { selected[category] = UI_COLLECTION_INVALIDINDEX; break; }
                selected[category] = index;
            }
        }
    }
    if (closing_ || !searchActive_ || activeQuery_ != query || searchInteractionRevision_ != revision || navigationCount_ != navigation) return;
    searchRefinementQuery_ = query; searchRefinementStatus_ = hr;
    searchRefinementInspected_ = true;
    searchRefinements_ = std::move(snapshot);
    searchRefinementSelected_ = SUCCEEDED(hr) ? selected : std::array<UINT, 3>{UI_COLLECTION_INVALIDINDEX, UI_COLLECTION_INVALIDINDEX, UI_COLLECTION_INVALIDINDEX};
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
    if ((command == LibraryDefault || command == LibraryOptimize || command == RibbonLibraryOptimizeMenu) &&
        !navigating_ && !namespaceDirty_) {
        // These installed controls are item galleries. Their selected index
        // comes from the retained native child's checked state, using the same
        // visible-row paths that route execution after separators/hidden rows.
        const auto snapshot = ribbonCommandChildren_.find(command);
        const auto paths = ribbonCommandPaths_.find(command);
        if (snapshot != ribbonCommandChildren_.end() && snapshot->second && paths != ribbonCommandPaths_.end()) {
            for (UINT row = 0; row < paths->second.size(); ++row) {
                const auto* entries = &snapshot->second->entries();
                const NamespaceSubcommandMetadata* selected = nullptr;
                for (const auto index : paths->second[row]) {
                    if (index >= entries->size()) { selected = nullptr; break; }
                    selected = &(*entries)[index];
                    entries = &selected->children;
                }
                if (!selected || !selected->children.empty() || FAILED(selected->stateStatus) ||
                    (selected->state & ECS_HIDDEN) || !(selected->state & ECS_CHECKED)) continue;
                if (state.selectedIndex != UI_COLLECTION_INVALIDINDEX) {
                    state.selectedIndex = UI_COLLECTION_INVALIDINDEX; break;
                }
                state.selectedIndex = row;
            }
        }
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
    case Copy: case CopyPath:
        state.enabled = selected && (selectionAttributes_ & SFGAO_CANCOPY); break;
    case Cut:
        state.enabled = selected && (selectionAttributes_ & SFGAO_CANMOVE); break;
    case RibbonDeleteMenu:
        state.enabled = selected && (selectionAttributes_ & SFGAO_CANDELETE); break;
    case Rename: state.enabled = selectionCount_ == 1 && (selectionAttributes_ & SFGAO_CANRENAME); break;
    case Properties: case RibbonPropertiesMenu:
        state.enabled = !selected || (selectionAttributes_ & SFGAO_HASPROPSHEET); break;
    case Open: case Edit: case Print: case RibbonOpenMenu: state.enabled = selected; break;
    case RibbonNewMenu:
        state.enabled=folderView_&&!navigating_;break;
    case NewText: case NewShortcut:
    case RibbonPowerShellMenu:
        state.enabled = physicalDirectory_ && !navigating_; break;
    case SizeColumns: {
        state.enabled = folderView_ && preferences_.view == ViewMode::Details;
        break;
    }
    case SelectAll: case Invert: state.enabled = folderView_ != nullptr; break;
    case SelectNone: state.enabled = selected; break;
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
    case SearchKindMenu: case SearchDateMenu: case SearchSizeMenu:
        state.enabled = searchActive_;
        if (searchActive_ && searchRefinementQuery_ == activeQuery_ && SUCCEEDED(searchRefinementStatus_))
            state.selectedIndex = searchRefinementSelected_[command == SearchKindMenu ? 0 : command == SearchDateMenu ? 1 : 2];
        break;
    case SaveSearch: case CloseSearch:
    case RibbonSearchOtherProperties: case RibbonSearchAdvancedMenu:
        state.enabled = searchActive_; break;
    case LibraryLocations: state.enabled = library_.valid(); break;
    case IncludeLibraryFolder: case RibbonLibraryShowInNavigation:
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

HRESULT ExplorerApp::appendSearchRefinementMenu(UINT command, HMENU menu, std::vector<std::wstring>* expressions) const {
    if (!expressions) return E_POINTER;
    if (!menu || GetMenuItemCount(menu) != 0) return E_INVALIDARG;
    const auto* choices = command == SearchKindMenu ? &kindChoices() : command == SearchDateMenu ? &dateChoices() :
        command == SearchSizeMenu ? &sizeChoices() : nullptr;
    if (!choices) return E_INVALIDARG;
    if (choices->empty()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    std::vector<std::wstring> snapshot;
    snapshot.reserve(choices->size());
    for (size_t index = 0; index < choices->size(); ++index) {
        const auto& choice = (*choices)[index];
        std::wstring label;
        for (const auto character : choice.label) { label += character; if (character == L'&') label += character; }
        const auto category = command == SearchKindMenu ? 0u : command == SearchDateMenu ? 1u : 2u;
        const bool checked = searchActive_ && searchRefinementQuery_ == activeQuery_ && SUCCEEDED(searchRefinementStatus_) &&
            searchRefinementSelected_[category] == index;
        if (!AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), 28000 + index, label.c_str())) {
            const auto error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        }
        snapshot.push_back(choice.expression);
    }
    *expressions = std::move(snapshot);
    return S_OK;
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
        displayedRecentSearches_ = recentSearches_;
        for (UINT i = 0; i < displayedRecentSearches_.size(); ++i) result.push_back({i,displayedRecentSearches_[i],false});
    } else {
        const auto* choices = command == SearchKindMenu ? &kindChoices() : command == SearchDateMenu ? &dateChoices() :
            command == SearchSizeMenu ? &sizeChoices() : nullptr;
        if (choices) for (UINT i = 0; i < choices->size(); ++i) {
            RibbonItem row{i,(*choices)[i].label,false};
            const auto category = command == SearchKindMenu ? 0u : command == SearchDateMenu ? 1u : 2u;
            row.checked = searchActive_ && searchRefinementQuery_ == activeQuery_ && SUCCEEDED(searchRefinementStatus_) &&
                searchRefinementSelected_[category] == i;
            row.checkable = true; result.push_back(std::move(row));
        }
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
            POINT point{}; UINT menuFlags = 0;
            auto placementRead = popupUiFlags(window_, TPM_RETURNCMD | TPM_RIGHTBUTTON, &menuFlags);
            if (SUCCEEDED(placementRead) && !GetCursorPos(&point)) placementRead = HRESULT_FROM_WIN32(GetLastError());
            if (FAILED(placementRead)) { DestroyMenu(menu); return placementRead; }
            const auto selected=TrackPopupMenuEx(menu,menuFlags,point.x,point.y,window_,nullptr);
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
        if (item >= displayedRecentSearches_.size()) return E_INVALIDARG;
        const auto query = displayedRecentSearches_[item];
        return startSearch(query,searchRecursive_);
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

HRESULT ExplorerApp::clearSearchHistory(const std::filesystem::path* ownedHeadlessPath) {
    if (ownedHeadlessPath) {
        const auto desktop = PrivateDesktop::current();
        if (!headless_ || !desktop || FAILED(desktop->verifyIsolation())) return E_ACCESSDENIED;
        if (ownedHeadlessPath->empty() || !ownedHeadlessPath->is_absolute()) return E_INVALIDARG;
    }
    if (ownedHeadlessPath || (!headless_ && searchSuggestionsAllowed_)) {
        // Publish the empty codec before changing visible suggestions. A
        // failed write must leave the current history available for retry.
        const auto persisted = saveSearchHistory(ownedHeadlessPath ? *ownedHeadlessPath : searchHistoryPath(), {});
        if (FAILED(persisted)) return persisted;
    }
    recentSearches_.clear();
    const auto hr = searchSuggestions_ ? searchSuggestions_->replace(recentSearches_) : S_OK;
    ribbon_.invalidate(RecentSearches);
    ribbon_.invalidate(RibbonClearSearchHistory);
    return hr;
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
            hr = clearSearchHistory();
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
    updateCommands();
    return hr;
}

int ExplorerApp::headlessVisual(const PrivateDesktop& desktop, const std::filesystem::path& screenshot,
                              const std::filesystem::path& report, const VisualScene& scene) {
    VisualCaptureOptions::CommandReadinessReadback readiness;
    VisualCaptureOptions options;
    const auto captureDeadline=GetTickCount64()+25000;
    const auto pumpScene=[&](const std::function<bool()>& ready,DWORD milliseconds) {
        const auto now=GetTickCount64();
        return now<captureDeadline&&pumpVisual(ready,static_cast<DWORD>(std::min<ULONGLONG>(milliseconds,captureDeadline-now)));
    };
    const auto readReadiness=[&] {
        readiness.namespaceGeneration=namespaceGeneration_;
        readiness.workerTasks=static_cast<UINT>(commandStateTasks_.size());
        readiness.selectionBatchPending=selectionStateBatch_!=nullptr;
        readiness.pendingCapabilities=0;
        for(const auto& [id,capability]:commandCapabilities_)if(capability.status==E_PENDING&&!capability.slowStateCompleted)
            ++readiness.pendingCapabilities;
    };
    auto failed = [&](int exitCode, const char* stage, HRESULT result = E_FAIL) {
        readReadiness();
        StaWorkerDiagnostics workers;
        const auto workerRead=staWorkerDiagnostics(&workers);
        std::ofstream diagnostic(report);
        diagnostic << "{\"headless\":true,\"failedStage\":\"" << stage << "\",\"hresult\":" << static_cast<long>(result)
            << ",\"commandReadiness\":{\"requested\":" << (readiness.requested?"true":"false")
            << ",\"ready\":" << (readiness.ready?"true":"false") << ",\"readHresult\":" << static_cast<long>(readiness.read)
            << ",\"waitMs\":" << readiness.waitMs
            << ",\"namespaceGeneration\":" << readiness.namespaceGeneration << ",\"workerTasks\":" << readiness.workerTasks
            << ",\"statePolls\":" << readiness.statePolls
            << ",\"selectionBatchPending\":" << (readiness.selectionBatchPending?"true":"false")
            << ",\"pendingCapabilities\":" << readiness.pendingCapabilities << '}'
            << ",\"nativeWorkers\":{\"readHresult\":" << static_cast<long>(workerRead)
            << ",\"pending\":" << workers.pending << ",\"completedThreads\":" << workers.completedThreads
            << ",\"creatorHandleUsers\":" << workers.creatorHandleUsers << ",\"otherHandleUsers\":" << workers.otherHandleUsers
            << ",\"desktopWindows\":" << workers.desktopWindows << ",\"creatorWindows\":" << workers.creatorWindows
            << ",\"otherWindows\":" << workers.otherWindows << '}'
            << ",\"searchDateMenu\":{\"resultReadHresult\":" << static_cast<long>(options.searchDateMenu.resultRead)
            << ",\"resultCount\":" << options.searchDateMenu.resultCount
            << ",\"expectedResults\":" << options.searchDateMenu.expectedResults
            << ",\"matchedIdentities\":" << options.searchDateMenu.matchedIdentities
            << ",\"unexpectedPaths\":" << options.searchDateMenu.unexpectedPaths
            << ",\"duplicateIdentities\":" << options.searchDateMenu.duplicateIdentities
            << ",\"submitReadHresult\":" << static_cast<long>(options.searchDateMenu.submitRead)
            << ",\"submitCount\":" << options.searchDateMenu.submitCount
            << ",\"recentCount\":" << options.searchDateMenu.recentCount
            << ",\"recentMatchesQuery\":" << (options.searchDateMenu.recentMatchesQuery?"true":"false")
            << ",\"scopeNavigationReadHresult\":" << static_cast<long>(options.searchDateMenu.scopeNavigationRead)
            << ",\"physicalScopeReady\":" << (options.searchDateMenu.physicalScopeReady?"true":"false")
            << ",\"savedInputUnchanged\":" << (options.searchDateMenu.savedInputUnchanged?"true":"false")
            << ",\"nativeViewChanged\":" << (options.searchDateMenu.nativeViewChanged?"true":"false")
            << ",\"scopePreserved\":" << (options.searchDateMenu.scopePreserved?"true":"false")
            << ",\"historyCommitted\":" << (options.searchDateMenu.historyCommitted?"true":"false")
            << ",\"factoryRetained\":" << (options.searchDateMenu.factoryRetained?"true":"false")
            << ",\"retainedFactoriesBefore\":" << options.searchDateMenu.retainedFactoriesBefore
            << ",\"retainedFactoriesAfter\":" << options.searchDateMenu.retainedFactoriesAfter
            << ",\"navigationDelta\":" << options.searchDateMenu.navigationDelta
            << ",\"recentEnabledReadHresult\":" << static_cast<long>(options.searchDateMenu.recentEnabledRead)
            << ",\"recentEnabled\":" << (options.searchDateMenu.recentEnabled?"true":"false") << '}'
            << ",\"searchDateExpansion\":" << nativePopupExpansionJson(options.searchDatePopup)
            << ",\"nativeRibbonProviders\":[";
        bool first=true;
        for(const auto& [id,capability]:commandCapabilities_) {
            diagnostic << (first?"":",") << "{\"command\":" << id << ",\"cachedReadHresult\":" << static_cast<long>(capability.status)
                << ",\"cachedEnabled\":" << (capability.enabled?"true":"false")
                << ",\"pending\":" << (capability.status==E_PENDING&&!capability.slowStateCompleted?"true":"false")
                << ",\"slowStateCompleted\":" << (capability.slowStateCompleted?"true":"false") << '}';first=false;
        }
        diagnostic << "]}";
        return exitCode;
    };
    if (!headless_ || !desktop.ready() || FAILED(desktop.verifyIsolation()) || !screenshot.is_absolute() ||
        !report.is_absolute() || scene.width<300 || scene.height<200) return failed(5,"isolation");
    if (!pumpScene([&]{return currentPidl_ && folderView_ && !navigating_;},15000)) return failed(6,"native-view-ready",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    dpi_=scene.dpi;
    if(scene.details!=preferences_.detailsPane||preferences_.previewPane)
        return failed(6,"visual-pane-setup",E_UNEXPECTED);
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
        ComPtr<IShellItem> expected;
        hr=SHCreateItemFromIDList(selected.get(),IID_PPV_ARGS(&expected));if(FAILED(hr))return failed(7,"selection-identity",hr);
        const auto available=pumpScene([&] {
            ComPtr<IShellItemArray> items;DWORD count=0;
            hr=folderView_->Items(SVGIO_ALLVIEW,IID_PPV_ARGS(&items));if(FAILED(hr)||!items)return false;
            hr=items->GetCount(&count);if(FAILED(hr))return false;
            for(DWORD index=0;index<count;++index) {
                ComPtr<IShellItem> item;int same=1;
                if(SUCCEEDED(items->GetItemAt(index,&item))&&item&&
                    SUCCEEDED(expected->Compare(item.Get(),SICHINT_CANONICAL,&same))&&same==0)return true;
            }
            return false;
        },5000);
        if(!available)return failed(7,"selection-child-ready",FAILED(hr)?hr:HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        hr=view_->SelectItem(ILFindLastID(selected.get()),SVSI_SELECT|SVSI_DESELECTOTHERS);
        if(FAILED(hr))return failed(7,"selection-apply",hr);
        const auto chosen=pumpScene([&] {
            ComPtr<IShellItemArray> current;DWORD count=0;ComPtr<IShellItem> item;int same=1;
            hr=selection(current);if(FAILED(hr)||!current)return false;
            hr=current->GetCount(&count);if(FAILED(hr)||count!=1)return false;
            hr=current->GetItemAt(0,&item);if(FAILED(hr)||!item)return false;
            hr=expected->Compare(item.Get(),SICHINT_CANONICAL,&same);return SUCCEEDED(hr)&&same==0;
        },5000);
        if(!chosen)return failed(7,"selection-exact-identity",FAILED(hr)?hr:HRESULT_FROM_WIN32(ERROR_TIMEOUT));
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
    if(scene.openSearchDateMenu&&(found->second!=RibbonSearchTab||scene.collapsed))return 2;
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
    const auto tabReady=pumpScene([&]{tabResult=ribbon_.selectTab(found->second);return SUCCEEDED(tabResult);},1000);
    if (!tabReady) {
        VisualCaptureOptions diagnosticOptions; diagnosticOptions.trimInvisibleFrame=true;
        VisualCaptureReport diagnosticCapture;
        captureWindowPng(desktop,window_,screenshot,diagnosticOptions,diagnosticCapture);
        writeVisualCaptureReport(report,diagnosticCapture);
        return 7;
    }
    updateCommands(); layout(); ribbon_.flush();
    const auto settle=GetTickCount64()+500;
    if(!pumpScene([&]{return GetTickCount64()>=settle;},1000))return failed(7,"native-layout-settle",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    if (FAILED(desktop.verifyIsolation())) return 5;
    SetActiveWindow(window_);applyWindowTheme(window_);layout();
    if(GetActiveWindow()!=window_)return failed(7,"private-active-frame",E_FAIL);
    std::filesystem::path ownedSearchScope;
    const auto sameFileIdentity=[](const FILE_ID_INFO& a,const FILE_ID_INFO& b) {
        return a.VolumeSerialNumber==b.VolumeSerialNumber&&
            std::equal(std::begin(a.FileId.Identifier),std::end(a.FileId.Identifier),std::begin(b.FileId.Identifier));
    };
    const auto fileIdentity=[](const std::filesystem::path& path,FILE_ID_INFO& value)->HRESULT {
        const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
        const auto read=GetFileInformationByHandleEx(handle,FileIdInfo,&value,sizeof(value));
        const auto result=read?S_OK:HRESULT_FROM_WIN32(GetLastError());CloseHandle(handle);return result;
    };
    if(scene.openSearchDateMenu) {
        const auto imported=pumpScene([&] {
            return searchActive_&&!navigating_&&!searchPresentationPending_&&!pendingDirectSearchTarget_&&
                !pendingLiveSearchTarget_&&!commandRefreshActive_&&!liveSearchDispatchActive_;
        },1000);
        if(!imported)return failed(7,"search-import-ready",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        const auto todayQuery=validateRelativeTodayQuery(activeQuery_);
        if(FAILED(todayQuery))return failed(7,"search-relative-today",todayQuery);
        options.searchDateMenu.relativeToday=true;
        if(!searchRecursive_||!searchScope_||!searchScopes_||searchScopeRules_.size()!=1||
            !searchScopeRules_[0].folder||!searchScopeRules_[0].recursive||searchScopeRules_[0].excluded)
            return failed(7,"search-owned-scope",E_INVALIDARG);
        const auto importedNavigation=navigationCount_;const auto importedRules=searchScopeRules_;
        ownedSearchScope=std::filesystem::path(nameOf(importedRules.front().folder.Get(),SIGDN_FILESYSPATH));
        const auto marker=GetFileAttributesW((ownedSearchScope.parent_path()/L".native-visual-fixture").c_str());
        const auto scopeAttributes=GetFileAttributesW(ownedSearchScope.c_str());
        if(!ownedSearchScope.is_absolute()||ownedSearchScope.filename()!=L"Search scope"||
            marker==INVALID_FILE_ATTRIBUTES||(marker&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            scopeAttributes==INVALID_FILE_ATTRIBUTES||!(scopeAttributes&FILE_ATTRIBUTE_DIRECTORY)||
            (scopeAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return failed(7,"search-owned-scope",E_INVALIDARG);
        const auto savedPath=ownedSearchScope.parent_path()/L"Owned search.search-ms";
        const auto savedAttributes=GetFileAttributesW(savedPath.c_str());
        if(savedAttributes==INVALID_FILE_ATTRIBUTES||(savedAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            return failed(7,"search-owned-input",E_INVALIDARG);
        const auto readSavedInput=[](const std::filesystem::path& path,FILE_ID_INFO& id,std::vector<BYTE>& bytes)->HRESULT {
            struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} file;
            file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file.value==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
            LARGE_INTEGER length{};
            if(!GetFileSizeEx(file.value,&length))return HRESULT_FROM_WIN32(GetLastError());
            if(length.QuadPart<=0||length.QuadPart>1024*1024)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            if(!GetFileInformationByHandleEx(file.value,FileIdInfo,&id,sizeof(id)))return HRESULT_FROM_WIN32(GetLastError());
            std::vector<BYTE> candidate(static_cast<size_t>(length.QuadPart));DWORD copied=0;
            if(!ReadFile(file.value,candidate.data(),static_cast<DWORD>(candidate.size()),&copied,nullptr))
                return HRESULT_FROM_WIN32(GetLastError());
            if(copied!=candidate.size())return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            bytes=std::move(candidate);return S_OK;
        };
        FILE_ID_INFO savedIdentity{};std::vector<BYTE> savedBytes;
        auto scopeRead=readSavedInput(savedPath,savedIdentity,savedBytes);
        if(FAILED(scopeRead))return failed(7,"search-owned-input",scopeRead);
        SavedSearchMetadata metadata;
        scopeRead=readSavedSearch(savedPath,&metadata);
        if(FAILED(scopeRead))return failed(7,"search-saved-metadata",scopeRead);
        scopeRead=validateRelativeTodayQuery(metadata.query);
        if(FAILED(scopeRead))return failed(7,"search-relative-today",scopeRead);
        if(!metadata.recursive||!metadata.scope||!metadata.scopes||metadata.scopeRules.size()!=1||
            !metadata.scopeRules.front().folder||!metadata.scopeRules.front().recursive||metadata.scopeRules.front().excluded)
            return failed(7,"search-saved-scope",E_INVALIDARG);
        const auto sameItem=[](IShellItem* a,IShellItem* b) {
            int comparison=1;return a&&b&&SUCCEEDED(a->Compare(b,SICHINT_CANONICAL,&comparison))&&comparison==0;
        };
        DWORD scopeCount=0;ComPtr<IShellItem> scopeItem;
        scopeRead=metadata.scopes->GetCount(&scopeCount);
        if(SUCCEEDED(scopeRead)&&scopeCount==1)scopeRead=metadata.scopes->GetItemAt(0,&scopeItem);
        if(FAILED(scopeRead)||scopeCount!=1||!sameItem(scopeItem.Get(),metadata.scope.Get())||
            !sameItem(metadata.scope.Get(),metadata.scopeRules.front().folder.Get())||
            !sameItem(metadata.scope.Get(),importedRules.front().folder.Get())||navigationCount_!=importedNavigation)
            return failed(7,"search-saved-scope",FAILED(scopeRead)?scopeRead:E_INVALIDARG);
        options.searchDateMenu.scopeRead=S_OK;
        const auto query=metadata.query;const auto rules=metadata.scopeRules;
        FILE_ID_INFO scopeIdentity{};scopeRead=fileIdentity(ownedSearchScope,scopeIdentity);
        if(FAILED(scopeRead))return failed(7,"search-owned-scope-identity",scopeRead);
        // The reference shows a typed query submitted from its real folder.
        // Opening the saved input imports the scope and unresolved condition;
        // now browse that physical scope through the ordinary navigation path.
        options.searchDateMenu.scopeNavigationRead=navigate(ownedSearchScope.wstring());
        if(FAILED(options.searchDateMenu.scopeNavigationRead))
            return failed(7,"search-scope-navigation",options.searchDateMenu.scopeNavigationRead);
        options.searchDateMenu.physicalScopeReady=pumpScene([&] {
            if(navigating_||!currentPidl_||!folderView_||searchActive_||searchBackground_||!physicalDirectory_||
                pendingDirectSearchTarget_||pendingLiveSearchTarget_||pendingLiveSearch_||searchPresentationPending_||
                commandRefreshActive_||liveSearchDispatchActive_||liveSearchPolicy_.waiting())return false;
            const auto navigation=navigationCount_;ComPtr<IShellItem> current;
            if(FAILED(currentFolder(current))||!sameItem(current.Get(),metadata.scope.Get()))return false;
            const auto path=std::filesystem::path(nameOf(current.Get(),SIGDN_FILESYSPATH));FILE_ID_INFO actual{};
            return _wcsicmp(path.c_str(),ownedSearchScope.c_str())==0&&SUCCEEDED(fileIdentity(path,actual))&&
                sameFileIdentity(scopeIdentity,actual)&&navigationCount_==navigation;
        },2500);
        if(!options.searchDateMenu.physicalScopeReady)
            return failed(7,"search-physical-scope",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        if(!searchRecursive_||!recentSearches_.empty()||!searchLocations_.empty())
            return failed(7,"search-submit-origin",E_UNEXPECTED);
        const auto samePidl=[](PCIDLIST_ABSOLUTE a,PCIDLIST_ABSOLUTE b) {
            if(!a||!b)return false;
            const auto bytes=ILGetSize(a);return bytes==ILGetSize(b)&&std::memcmp(a,b,bytes)==0;
        };
        Pidl beforeLocation(ILCloneFull(currentPidl_.get()));
        if(!beforeLocation)return failed(7,"search-submit-snapshot",E_OUTOFMEMORY);
        std::vector<Pidl> history;
        for(const auto& entry:history_) {
            Pidl copy(ILCloneFull(entry.get()));if(!copy)return failed(7,"search-submit-snapshot",E_OUTOFMEMORY);
            history.push_back(std::move(copy));
        }
        const auto historyIndex=historyIndex_;const auto navigation=navigationCount_;
        options.searchDateMenu.retainedFactoriesBefore=static_cast<UINT>(searchLocations_.size());
        if(historyIndex<0||static_cast<size_t>(historyIndex)+1!=history.size())return failed(7,"search-submit-history",E_UNEXPECTED);
        setSearchText(query);
        const auto editLength=GetWindowTextLengthW(search_);
        if(editLength<0||static_cast<size_t>(editLength)!=query.size()||!recentSearches_.empty())
            return failed(7,"search-submit-literal",E_UNEXPECTED);
        std::wstring literal(static_cast<size_t>(editLength)+1,L'\0');
        const auto editRead=GetWindowTextW(search_,literal.data(),editLength+1);literal.resize(static_cast<size_t>(editRead));
        if(literal!=query)return failed(7,"search-submit-literal",E_UNEXPECTED);
        // Submit once. Only the production route creates the native factory,
        // commits its completed navigation/history and records the real MRU.
        options.searchDateMenu.submitCount=1;
        options.searchDateMenu.submitRead=execute(Search);
        if(FAILED(options.searchDateMenu.submitRead))return failed(7,"search-submit",options.searchDateMenu.submitRead);
        const auto submitted=pumpScene([&] {
            return searchActive_&&folderView_&&!navigating_&&!pendingDirectSearchTarget_&&!pendingLiveSearchTarget_&&
                !pendingLiveSearch_&&!liveSearchPolicy_.waiting()&&!searchPresentationPending_&&recentSearches_.size()==1;
        },3000);
        options.searchDateMenu.recentCount=static_cast<UINT>(recentSearches_.size());
        options.searchDateMenu.recentMatchesQuery=recentSearches_.size()==1&&recentSearches_.front()==query&&
            liveSearchPolicy_.committedLiteral()==query;
        options.searchDateMenu.navigationDelta=navigationCount_-navigation;
        options.searchDateMenu.historyCommitted=history_.size()==history.size()+1&&historyIndex_==historyIndex+1&&
            std::equal(history.begin(),history.end(),history_.begin(),[&](const Pidl& a,const Pidl& b){return samePidl(a.get(),b.get());});
        options.searchDateMenu.nativeViewChanged=!samePidl(beforeLocation.get(),currentPidl_.get())&&
            options.searchDateMenu.navigationDelta==1;
        options.searchDateMenu.retainedFactoriesAfter=static_cast<UINT>(searchLocations_.size());
        const auto inspectionRevision=searchInteractionRevision_;const auto inspectionNavigation=navigationCount_;
        const auto currentRules=searchScopeRules_;const auto currentScopes=searchScopes_;const auto afterFolderView=folderView_;
        Pidl finalLocation(currentPidl_?ILCloneFull(currentPidl_.get()):nullptr);
        Pidl finalScope(searchScope_?ILCloneFull(searchScope_.get()):nullptr);
        Pidl finalHistory(historyIndex_>=0&&static_cast<size_t>(historyIndex_)<history_.size()?
            ILCloneFull(history_[static_cast<size_t>(historyIndex_)].get()):nullptr);
        if(!finalLocation||!finalScope||!finalHistory)return failed(7,"search-submit-snapshot",E_OUTOFMEMORY);
        ComPtr<IShellItem> currentScopeItem;
        scopeRead=SHCreateItemFromIDList(finalScope.get(),IID_PPV_ARGS(&currentScopeItem));
        DWORD currentScopeCount=0;ComPtr<IShellItem> currentScope;
        if(SUCCEEDED(scopeRead)&&currentScopes)scopeRead=currentScopes->GetCount(&currentScopeCount);
        if(SUCCEEDED(scopeRead)&&currentScopeCount==1)scopeRead=currentScopes->GetItemAt(0,&currentScope);
        // Retain scope vectors/interfaces across public COM comparisons, then
        // reject any newer navigation or interaction after their readbacks.
        const bool sameRules=currentRules.size()==rules.size()&&std::equal(rules.begin(),rules.end(),currentRules.begin(),[](const auto& a,const auto& b) {
                int same=1;return a.recursive==b.recursive&&a.excluded==b.excluded&&a.folder&&b.folder&&
                    SUCCEEDED(a.folder->Compare(b.folder.Get(),SICHINT_CANONICAL,&same))&&same==0;
            });
        options.searchDateMenu.scopePreserved=SUCCEEDED(scopeRead)&&currentScopeCount==1&&sameRules&&
            sameItem(currentScopeItem.Get(),metadata.scope.Get())&&sameItem(currentScope.Get(),metadata.scope.Get())&&
            searchActive_&&searchRecursive_&&activeQuery_==query&&SUCCEEDED(validateRelativeTodayQuery(activeQuery_))&&
            searchInteractionRevision_==inspectionRevision&&navigationCount_==inspectionNavigation&&
            folderView_.Get()==afterFolderView.Get()&&samePidl(finalLocation.get(),currentPidl_.get());
        if(searchLocations_.size()==1) {
            const auto& factory=searchLocations_.front();
            options.searchDateMenu.factoryRetained=factory.location&&factory.scope&&factory.query==query&&factory.recursive&&
                samePidl(factory.scope.get(),finalScope.get())&&samePidl(factory.completedLocation.get(),finalLocation.get())&&
                samePidl(factory.historyLocation.get(),finalHistory.get());
        }
        options.searchDateMenu.historyCommitted=options.searchDateMenu.historyCommitted&&samePidl(finalHistory.get(),finalLocation.get());
        FILE_ID_INFO afterSavedIdentity{};std::vector<BYTE> afterSavedBytes;
        const auto savedRead=readSavedInput(savedPath,afterSavedIdentity,afterSavedBytes);
        options.searchDateMenu.savedInputUnchanged=SUCCEEDED(savedRead)&&sameFileIdentity(savedIdentity,afterSavedIdentity)&&savedBytes==afterSavedBytes;
        if(!submitted||!options.searchDateMenu.recentMatchesQuery||!options.searchDateMenu.nativeViewChanged||
            !options.searchDateMenu.scopePreserved||!options.searchDateMenu.historyCommitted||
            !options.searchDateMenu.factoryRetained||!options.searchDateMenu.savedInputUnchanged||
            options.searchDateMenu.retainedFactoriesBefore!=0||options.searchDateMenu.retainedFactoriesAfter!=1)
            return failed(7,"search-submit-native-navigation",submitted?E_UNEXPECTED:HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    readiness.requested=true;
    const auto stateWaitStarted=GetTickCount64();
    auto nextStatePoll=stateWaitStarted;
    ULONGLONG stableSince=0,stableGeneration=namespaceGeneration_;
    const auto stateSettled=[&] {
        return currentPidl_&&folderView_&&!navigating_&&!selectionStateDirty_&&!namespaceDirty_&&
            !deferredUpdateQueued_&&!commandRefreshActive_&&!commandRefreshPending_&&
            !commandStatesCancelPending_&&!commandItemsRefreshPending_&&!commandStatesPending();
    };
    const auto stateReady=pumpScene([&] {
        const auto now=GetTickCount64();
        if(now>=nextStatePoll&&!commandRefreshActive_&&!navigating_) {
            // Drive the same completion/retry policy as WM_TIMER 4 and the
            // benchmark. Native modal pumping need not deliver a low-priority
            // timer before this bounded capture phase ends.
            nextStatePoll=now+50;++readiness.statePolls;pollCommandStates();
        }
        if(!stateSettled()||stableGeneration!=namespaceGeneration_) {
            stableSince=0;stableGeneration=namespaceGeneration_;return false;
        }
        if(!stableSince)stableSince=now;
        return now-stableSince>=50;
    },10000);
    readiness.waitMs=GetTickCount64()-stateWaitStarted;
    readReadiness();readiness.ready=stateReady;readiness.read=stateReady?S_OK:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if(!stateReady)return failed(7,"native-command-state-ready",readiness.read);
    ribbon_.flush();
    if(scene.openSearchDateMenu) {
        PROPVARIANT enabled{};BOOL value=FALSE;
        options.searchDateMenu.recentEnabledRead=ribbon_.framework()->GetUICommandProperty(RecentSearches,UI_PKEY_Enabled,&enabled);
        if(SUCCEEDED(options.searchDateMenu.recentEnabledRead))options.searchDateMenu.recentEnabledRead=PropVariantToBoolean(enabled,&value);
        PropVariantClear(&enabled);options.searchDateMenu.recentEnabled=value!=FALSE;
        if(FAILED(options.searchDateMenu.recentEnabledRead)||!options.searchDateMenu.recentEnabled)
            return failed(7,"search-recent-enabled",FAILED(options.searchDateMenu.recentEnabledRead)?options.searchDateMenu.recentEnabledRead:E_UNEXPECTED);
        const auto recentSnapshot=recentSearches_;
        options.searchDateMenu.recentCount=static_cast<UINT>(recentSnapshot.size());
        options.searchDateMenu.recentMatchesQuery=recentSnapshot.size()==1&&recentSnapshot.front()==activeQuery_&&
            liveSearchPolicy_.committedLiteral()==activeQuery_;
        if(!options.searchDateMenu.recentMatchesQuery)return failed(7,"search-recent-snapshot",E_UNEXPECTED);
    }
    // Theme/layout invalidation must finish through the real window paint
    // procedures. On Windows 10 PrintWindow can copy an existing surface
    // without sending WM_PRINT; an unpainted owner-drawn child stays blank.
    if(!RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|
        RDW_ALLCHILDREN|RDW_UPDATENOW))return failed(7,"native-repaint",HRESULT_FROM_WIN32(GetLastError()));
    const auto painted=pumpScene([&] {
        return !GetUpdateRect(window_,nullptr,FALSE) &&
            (!ribbonCollapse_ || !GetUpdateRect(ribbonCollapse_,nullptr,FALSE));
    },1000);
    if(!painted || !GdiFlush())return failed(7,"native-paint-settle",E_FAIL);
    if(!stateSettled()||namespaceGeneration_!=readiness.namespaceGeneration)
        return failed(7,"native-command-state-changed",HRESULT_FROM_WIN32(ERROR_RETRY));
    options.trimInvisibleFrame=true; options.layoutDpi=scene.dpi;
    options.commandReadiness=readiness;
    if(scene.openSearchDateMenu) {
        const auto todayQuery=validateRelativeTodayQuery(activeQuery_);
        if(FAILED(todayQuery))return failed(7,"search-relative-today",todayQuery);
        const auto& scope=ownedSearchScope;
        const std::array<std::filesystem::path,2> expectedPaths{scope/L"Today.txt",scope/L"Child"/L"Today child.txt"};
        options.searchDateMenu.expectedResults=static_cast<UINT>(expectedPaths.size());
        std::array<FILE_ID_INFO,2> expected{};
        for(size_t index=0;index<expected.size();++index) {
            const auto read=fileIdentity(expectedPaths[index],expected[index]);
            if(FAILED(read))return failed(7,"search-owned-identity",read);
        }
        HRESULT results=E_PENDING;
        const auto exactResults=pumpScene([&] {
            options.searchDateMenu.matchedIdentities=0;
            options.searchDateMenu.unexpectedPaths=0;
            options.searchDateMenu.duplicateIdentities=0;
            ComPtr<IShellItemArray> items;
            results=folderView_->Items(SVGIO_ALLVIEW,IID_PPV_ARGS(&items));if(FAILED(results)||!items)return false;
            DWORD count=0;results=items->GetCount(&count);options.searchDateMenu.resultCount=count;
            if(FAILED(results)||count!=expected.size())return false;
            std::array<bool,2> matched{};
            for(DWORD index=0;index<count;++index) {
                ComPtr<IShellItem> item;results=items->GetItemAt(index,&item);if(FAILED(results)||!item)return false;
                const auto path=std::filesystem::path(nameOf(item.Get(),SIGDN_FILESYSPATH));
                // These are the two independently constructed owned files.
                // Reject another route before opening any result for attributes.
                if(std::none_of(expectedPaths.begin(),expectedPaths.end(),[&](const auto& expectedPath){
                    return _wcsicmp(expectedPath.c_str(),path.c_str())==0;})) {
                    ++options.searchDateMenu.unexpectedPaths;return false;
                }
                FILE_ID_INFO actual{};results=fileIdentity(path,actual);if(FAILED(results))return false;
                bool found=false;
                for(size_t wanted=0;wanted<expected.size();++wanted)if(sameFileIdentity(actual,expected[wanted])) {
                    if(matched[wanted]){++options.searchDateMenu.duplicateIdentities;return false;}
                    matched[wanted]=true;found=true;++options.searchDateMenu.matchedIdentities;
                }
                if(!found)return false;
            }
            return std::all_of(matched.begin(),matched.end(),[](bool value){return value;});
        },5000);
        options.searchDateMenu.resultRead=exactResults?S_OK:FAILED(results)?results:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(!exactResults)return failed(7,"search-native-identities",options.searchDateMenu.resultRead);
        std::vector<std::wstring> expectedRows;
        for(const auto& item:ribbonItems(SearchDateMenu))expectedRows.push_back(item.label);
        options.searchDateMenu.requested=true;
        options.searchDateMenu.expectedRows=static_cast<UINT>(expectedRows.size());
        if(GetTickCount64()+5500>=captureDeadline)return failed(7,"search-menu-budget",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        options.searchDateMenu.read=ribbon_.expandSearchDateMenu(expectedRows,options.searchDateMenu.matchedRows,
            &options.searchDatePopup);
        options.searchDateMenu.expanded=SUCCEEDED(options.searchDateMenu.read);
        if(FAILED(options.searchDateMenu.read))return failed(7,"search-date-menu",options.searchDateMenu.read);
    }
    options.ribbonFramework=ribbon_.framework();
    options.nativeRibbonFramework=ribbon_.nativeFramework();
    options.layoutGalleryNativeCommand=ribbon_.nativeCommandId(RibbonLayoutGallery);
    options.ribbonLayout=ribbon_.layout()==RibbonLayout::InstalledWindows10?L"InstalledWindows10":L"Authored";
    options.installedRibbonStatus=ribbon_.installedLayoutStatus();
    options.ribbonFeaturesRead=true;
    options.ribbonFeatures=ribbon_.features();
    for(const auto& [command,capability]:commandCapabilities_) {
        VisualCaptureOptions::ProviderReadback provider;
        provider.command=command;provider.selectedCount=selectionCount_;
        provider.cachedRead=capability.status;provider.cachedEnabled=capability.enabled;
        provider.cachedChecked=capability.checked;provider.cachedNativeState=capability.native.state;
        provider.pending=capability.status==E_PENDING&&!capability.slowStateCompleted;
        provider.slowStateCompleted=capability.slowStateCompleted;
        options.ribbonProviders.push_back(provider);
    }
    if(found->second==RibbonShareTab) {
        constexpr std::pair<UINT,std::wstring_view> commands[]{
            {RibbonEmail,L"Windows.email"},{RibbonSpecificPeople,L"Windows.ShareSpecificUsers"},{RibbonStopSharing,L"Windows.SharePrivate"}};
        for(const auto& [command,key]:commands) {
            auto provider=std::find_if(options.ribbonProviders.begin(),options.ribbonProviders.end(),
                [&](const auto& entry){return entry.command==command;});
            if(provider==options.ribbonProviders.end()) {
                VisualCaptureOptions::ProviderReadback missing;missing.command=command;missing.selectedCount=selectionCount_;
                missing.cachedRead=HRESULT_FROM_WIN32(ERROR_NOT_FOUND);options.ribbonProviders.push_back(missing);
                provider=std::prev(options.ribbonProviders.end());
            }
            NamespaceCommandState actual;
            provider->nativeRead=namespaceActions_.queryCommandState(key,&actual,NamespaceMenuScope::Selection);
            if(SUCCEEDED(provider->nativeRead))provider->nativeState=actual.state;
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
