#include "explorer/app_commands.hpp"

#include <array>
#include <algorithm>

namespace explorer {
namespace {
using Route = AppCommandRoute;
using Scope = NamespaceMenuScope;
using Action = NamespaceAction;
constexpr AppCommandBinding host(UINT id,std::wstring_view key = {},unsigned requirements = RequireFolderView,
                                 Scope scope = Scope::Selection) {
    return {id,Route::Host,Action::Count,key,scope,requirements};
}
constexpr AppCommandBinding native(UINT id,std::wstring_view key,unsigned requirements = RequireFolderView,
                                   Scope scope = Scope::Background) {
    return {id,Route::CommandStore,Action::Count,key,scope,requirements};
}
constexpr AppCommandBinding action(UINT id,Action target,unsigned requirements = RequireFolderView) {
    return {id,Route::NamespaceAction,target,{},Scope::Selection,requirements};
}
constexpr unsigned selected = RequireFolderView | RequireSelection;
constexpr unsigned directory = RequireFolderView | RequirePhysicalDirectory;
constexpr unsigned library = RequireFolderView | RequireLibrary;
constexpr unsigned writableLibrary = library | RequireWritableLibrary;
constexpr unsigned search = RequireFolderView | RequireSearch;
constexpr unsigned searchBackground = RequireFolderView | RequireSearchBackground;
constexpr unsigned computer = RequireFolderView | RequireComputer;
constexpr unsigned network = RequireFolderView | RequireNetwork;
constexpr std::array catalog{
    host(Back),host(Forward),host(Up),host(Refresh),host(Address),host(AddressList),host(Search),host(FileMenu),
    host(NewWindow,L"Windows.location.opennewwindow"),host(RibbonNewProcess,L"Windows.location.opennewprocess"),
    host(Close,L"Windows.closewindow",RequireNone),
    host(Copy,L"Windows.copy",selected | RequireCopy),host(Cut,L"Windows.cut",selected | RequireMove),
    native(Paste,L"Windows.paste"),native(PasteShortcut,L"Windows.pastelink"),
    host(CopyPath,L"Windows.copyaspath",selected),native(CopyTo,L"Windows.CopyToMenu",selected | RequireCopy,Scope::Selection),
    native(MoveTo,L"Windows.MoveToMenu",selected | RequireMove,Scope::Selection),native(Delete,L"Windows.recycle",selected | RequireDelete,Scope::Selection),
    native(PermanentDelete,L"Windows.PermanentDelete",selected | RequireDelete,Scope::Selection),
    host(Rename,L"Windows.rename",RequireFolderView | RequireSingleSelection | RequireRename),
    native(NewFolder,L"Windows.newfolder"),host(NewText,{},directory),host(NewShortcut,{},directory),
    native(NewItems,L"Windows.newitem"),host(Properties,L"Windows.properties"),
    host(Open,L"Windows.open",selected),host(Edit,L"Windows.edit",selected),host(Print,L"Windows.print",selected),
    host(Pin,L"Windows.PinToHome",RequireFolderView | RequireSingleSelection),
    native(Sharing,L"Windows.ModernShare",selected,Scope::Selection),host(SharingProperties,{},RequireFolderView),
    action(Security,Action::AdvancedSecurity),action(FileHistory,Action::FileHistory),
    host(SelectAll,L"Windows.selectall"),host(SelectNone,L"Windows.selectnone",selected),host(Invert,L"Windows.invertselection"),
    host(FolderOptions,L"Windows.folderoptions",RequireNone,Scope::Background),
    host(RibbonFolderOptions,L"Windows.folderoptions",RequireNone,Scope::Background),
    host(MapDrive,L"Windows.MapNetworkDrive",RequireNone,Scope::Background),
    host(DisconnectDrive,L"Windows.DisconnectNetworkDrive",RequireNone,Scope::Background),
    native(Terminal,L"Windows.location.Powershell",directory),
    native(RibbonPowerShellAdmin,L"Windows.location.PowershellAsAdmin",directory),
    host(NavigationPane,L"Windows.navpane"),host(PreviewPane,L"Windows.readingpane"),host(DetailsPane,L"Windows.previewpane"),
    host(HiddenItems,L"Windows.ShowHiddenFiles"),host(Extensions,L"Windows.ShowFileExtensions"),host(Collapse,{},RequireNone),
    host(Checkboxes,L"Windows.SelectionCheckboxes"),
    native(HideSelected,L"Windows.HideSelected",selected | RequireFilesystemSelection,Scope::Selection),
    native(SortMenu,L"Windows.SortByColumn"),native(GroupMenu,L"Windows.GroupByColumn"),
    native(ColumnsMenu,L"Windows.AddColumns",RequireFolderView | RequireDetailsView),
    host(SizeColumns,L"Windows.SizeAllColumns",RequireFolderView | RequireDetailsView),
    host(SortName),host(SortDate),host(SortType),host(SortSize),host(SortAscending,L"Windows.SortAscending"),
    host(SortDescending,L"Windows.SortDescending"),host(GroupNone),host(GroupName),host(GroupDate),host(GroupType),host(GroupSize),
    host(QuickAccess),host(ThisPC),host(Desktop),host(Documents),host(Downloads),host(Pictures),host(Music),host(Videos),
    host(Network),host(RecycleBin),host(Libraries),host(HistoryMenu),host(ViewMenu),
    native(Undo,L"Windows.undo"),native(Redo,L"Windows.redo"),
    AppCommandBinding{Zip,Route::NativeZip,Action::Count,L"Windows.zip",Scope::Selection,selected | RequireFilesystemSelection},
    native(Extract,L"Windows.CompressedFolder.extract",RequireFolderView | RequireArchive,Scope::Selection),
    host(FocusSearch),host(FocusNext),host(FocusPrevious),host(Fullscreen,{},RequireNone),
    host(CloseSearch,L"Windows.SearchCloseTab",searchBackground),
    host(SearchSubfolders,L"Windows.SearchOptionDeep",search),host(SearchCurrent,L"Windows.SearchOptionShallow",search),
    host(RecentSearches,L"Windows.SearchMru",search),host(SaveSearch,L"Windows.SearchSave",search),
    host(SearchKindMenu,L"Windows.SearchFilterKind",search),host(SearchDateMenu,L"Windows.SearchFilterDate",search),
    host(SearchSizeMenu,L"Windows.SearchFilterSize",search),
    native(OpenFileLocation,L"Windows.SearchOpenLocation",searchBackground | RequireSelection,Scope::Selection),
    host(QuickAccessMenu,{},RequireNone),host(QuickAccessPlacement,{},RequireNone),host(QuickAccessReset,{},RequireNone),
    host(NewLibrary,{},RequireNone),host(IncludeLibraryFolder,L"Windows.LibraryIncludeInLibrary",writableLibrary),
    host(LibraryLocations,L"Windows.LibraryManageLibrary",library,Scope::Background),
    native(LibraryDefault,L"Windows.LibraryDefaultSaveLocation",writableLibrary),
    native(LibraryOptimize,L"Windows.LibraryOptimizeLibraryFor",writableLibrary),
    native(RibbonLibraryOptimizeMenu,L"Windows.LibraryOptimizeLibraryFor",writableLibrary),
    native(RibbonLibraryChangeIcon,L"Windows.LibraryChangeIcon",writableLibrary),
    host(RibbonLibraryShowInNavigation,L"Windows.LibraryShowInNavPane",writableLibrary,Scope::Background),
    native(RibbonResetLibrary,L"Windows.LibraryRestoreDefaults",writableLibrary),
    host(RibbonNewLibraryMenu,{},RequireNone),
    host(ExpandAncestors),host(RibbonExpandToCurrent,L"Windows.NavPaneExpandToCurrentFolder"),
    host(RibbonShowAllFolders,L"Windows.NavPaneShowAllFolders"),host(RibbonShowLibraries,L"Windows.NavPaneShowLibraries"),
    host(RibbonSearchThisPC,{},search),host(RibbonSearchOtherProperties,L"Windows.SearchFilterMoreProperties",search),
    native(RibbonSearchContents,L"Windows.SearchOptionContents",searchBackground),
    native(RibbonSearchSystemFiles,L"Windows.SearchOptionSystem",searchBackground),
    native(RibbonSearchZipFiles,L"Windows.SearchOptionCompressed",searchBackground),
    native(RibbonChangeIndexedLocations,L"Windows.ChangeIndexedLocations",RequireNone),
    native(RibbonSystemProperties,L"Windows.SystemProperties",computer),
    native(RibbonUninstallProgram,L"Windows.AddRemovePrograms",computer),
    native(RibbonManageComputer,L"Windows.Computer.Manage",computer),
    native(RibbonAddNetworkLocation,L"Windows.AddNetworkLocation",computer),
    native(RibbonAccessMedia,L"Windows.AddMediaServer",computer),
    native(RibbonRemoveMediaServer,L"Windows.RemoveMediaServer",computer | RequireSingleSelection,Scope::Selection),
    native(RibbonOpenSearchViewSite,L"Windows.OpenSearchViewSite",searchBackground),
    native(RibbonGroupSortAscending,L"Windows.SortGroupsAscending"),
    native(RibbonGroupSortDescending,L"Windows.SortGroupsDescending"),
    native(RibbonAddNetworkDevice,L"Windows.AddDevice",network),
    native(RibbonSearchActiveDirectory,L"Windows.SearchActiveDirectory",network),
    native(RibbonNetworkSharingCenter,L"Windows.NetworkAndSharing",network),
    native(RibbonDeviceWebpage,L"Windows.NetworkViewDeviceWebpage",RequireFolderView | RequireSingleSelection,Scope::Selection),
    native(RibbonConnectRemotePrinter,L"Windows.ViewRemotePrinters",RequireFolderView | RequireSingleSelection,Scope::Selection),
    native(RibbonShortcutOpenLocation,L"Windows.Shortcut.opencontaining",selected,Scope::Selection),
    native(RibbonLibraryPublicSaveLocation,L"Windows.LibraryPublicSaveLocation",writableLibrary),
    native(RibbonRemoveProperties,L"Windows.removeproperties",selected,Scope::Selection),
    native(RibbonDeleteConfirmation,L"Windows.ToggleRecycleConfirmations"),
    native(RibbonRunAsAnotherUser,L"Windows.runasuser",selected,Scope::Selection),
    native(RibbonCloudBackup,L"Windows.CloudBackup",RequireFolderView,Scope::Selection),
    native(RibbonBackup,L"Windows.Backup",RequireFolderView | RequireDriveRoot,Scope::Selection),
    native(RibbonConnectRemote,L"Windows.remotedesktop",RequireFolderView | RequireSingleSelection,Scope::Selection),
    native(RibbonAddToFavorites,L"Windows.AddToFavorites",RequireFolderView | RequireSingleSelection,Scope::Selection),
    native(RibbonPinToStart,L"Windows.pintostartscreen",RequireFolderView | RequireSingleSelection,Scope::Selection),
    native(RibbonPinToTaskbar,L"Windows.taskbarpin",RequireFolderView | RequireSingleSelection,Scope::Selection),
    native(RibbonAutoPlay,L"Windows.Autoplay",RequireFolderView | RequireDriveRoot,Scope::Selection),
    native(RibbonFinishBurning,L"Windows.FinishBurn",RequireFolderView | RequireDriveRoot,Scope::Selection),
    native(RibbonEraseDisc,L"Windows.EraseDisc",RequireFolderView | RequireDriveRoot,Scope::Selection),
    native(RibbonExtractToGallery,L"Windows.CompressedFile.ExtractTo",RequireFolderView | RequireArchive,Scope::Selection),
    native(RibbonRecycleProperties,L"Windows.RecycleBin.properties"),
    native(RibbonOpenWith,L"Windows.OpenWith",selected,Scope::Selection),
    host(RibbonOpenSettings,{},RequireNone),host(RibbonHelpButton,L"Windows.help",RequireNone),
    host(RibbonHelp,L"Windows.help",RequireNone),host(RibbonAbout,L"Windows.aboutWindows",RequireNone),
    action(RibbonFormatDrive,Action::FormatDrive),action(RibbonOptimizeDrive,Action::OptimizeDrive),
    action(RibbonDiskCleanup,Action::CleanUpDrive),action(RibbonEjectDrive,Action::EjectDrive),
    action(RibbonBitLocker,Action::BitLocker),action(RibbonMountDiscImage,Action::MountDiscImage),
    action(RibbonBurnDiscImage,Action::BurnDiscImage),action(RibbonRotateLeft,Action::RotateLeft),
    action(RibbonRotateRight,Action::RotateRight),action(RibbonSlideShow,Action::SlideShow),
    action(RibbonSetBackground,Action::SetWallpaper),action(RibbonRestoreSelected,Action::RestoreSelected),
    action(RibbonRestoreAll,Action::RestoreAll),action(RibbonEmptyRecycleBin,Action::EmptyRecycleBin),
    action(RibbonRunAsAdministrator,Action::RunAsAdministrator),action(RibbonTroubleshootCompatibility,Action::TroubleshootCompatibility),
    action(RibbonIncludeInLibrary,Action::IncludeInLibrary),action(RibbonAlwaysAvailableOffline,Action::AlwaysAvailableOffline),
    action(RibbonWorkOffline,Action::WorkOffline),action(RibbonSyncOffline,Action::SyncOffline),
    action(RibbonMapAsDrive,Action::MapAsDrive),action(RibbonSpecificPeople,Action::ShareSpecificPeople),
    action(RibbonStopSharing,Action::RemoveAccess),action(RibbonEmail,Action::Email),action(RibbonFax,Action::Fax),
    action(RibbonBurnDisc,Action::BurnToDisc),action(RibbonPlay,Action::Play),action(RibbonPlayAll,Action::PlayAll),
    action(RibbonAddToPlaylist,Action::AddToPlaylist),action(RibbonCastToDevice,Action::CastToDevice),
    host(RibbonCopyToDesktop,{},selected | RequireCopy),host(RibbonCopyToDocuments,{},selected | RequireCopy),
    host(RibbonCopyToDownloads,{},selected | RequireCopy),host(RibbonMoveToDesktop,{},selected | RequireMove),
    host(RibbonMoveToDocuments,{},selected | RequireMove),host(RibbonMoveToDownloads,{},selected | RequireMove),
    host(RibbonFrequentPlaces,{},RequireNone),host(RibbonLayoutGallery),host(RibbonNavigationMenu),
    host(RibbonArrangeMenu),host(RibbonEasyAccessMenu),host(RibbonOptionsMenu),host(RibbonOpenMenu,L"Windows.open",selected),
    host(RibbonNewMenu,L"Windows.newitem",RequireFolderView,Scope::Background),host(RibbonDeleteMenu,L"Windows.RibbonDelete",selected | RequireDelete),
    host(RibbonSearchAdvancedMenu,{},searchBackground),host(RibbonDateMenu,{},search),host(RibbonSizeMenu,{},search),
    host(RibbonKindMenu,{},search),host(RibbonMapMenu,{},RequireNone),host(RibbonHelpMenu,{},RequireNone),
    host(RibbonHistoryMenu),host(RibbonPowerShellMenu,L"Windows.location.Powershell",directory),
    native(RibbonMoveMenu,L"Windows.MoveToMenu",selected | RequireMove,Scope::Selection),
    native(RibbonCopyMenu,L"Windows.CopyToMenu",selected | RequireCopy,Scope::Selection),
    host(RibbonSearchAgainMenu,{},search),host(RibbonPropertiesMenu,L"Windows.properties"),
    host(RibbonNewWindowMenu,L"Windows.location.opennewwindow"),host(RibbonQuickAccess,{},RequireNone),
    host(RibbonClearSearchHistory,{},RequireNone)
};
constexpr bool uniqueCatalog() {
    for (size_t left = 0; left < catalog.size(); ++left)
        for (size_t right = left + 1; right < catalog.size(); ++right)
            if (catalog[left].command == catalog[right].command) return false;
    return true;
}
static_assert(uniqueCatalog());

const wchar_t* staticVerb(UINT command) {
    switch (command) {
    case Open: case RibbonOpenMenu: return L"open";
    case Edit: return L"edit";
    case Print: return L"print";
    case Pin: return L"pintohome";
    case RibbonRemoveProperties: return L"removeproperties";
    case RibbonRunAsAnotherUser: return L"runasuser";
    case RibbonRunAsAdministrator: return L"runas";
    case RibbonAddToPlaylist: return L"enqueue";
    case RibbonManageComputer: return L"manage";
    case RibbonConnectRemote: return L"remotedesktop";
    case RibbonPinToStart: return L"pintostartscreen";
    default: return nullptr;
    }
}
// These controls call documented host/view APIs. Their registered Windows key
// supplies presentation on systems where it has no public state interface.
// The host still applies the actual view, SFGAO and operation-specific state.
bool ownsHostState(UINT command) noexcept {
    switch(command) {
    case NewWindow: case RibbonNewProcess: case RibbonNewWindowMenu:
    case Copy: case Cut: case Rename: case Properties: case RibbonPropertiesMenu:
    case NavigationPane: case PreviewPane: case DetailsPane:
    case RibbonHelpButton: case RibbonHelp: case RibbonAbout:
    case SaveSearch: case RecentSearches: case IncludeLibraryFolder: return true;
    default: return false;
    }
}
bool hasSeparateHostState(UINT command) noexcept {
    // App-owned saved-query/history/library operations differ from the
    // selected-item/native-menu action whose key provides their artwork.
    return command==SaveSearch || command==RecentSearches || command==IncludeLibraryFolder;
}
} // namespace

std::span<const AppCommandBinding> appCommandCatalog() noexcept { return catalog; }
std::optional<AppCommandBinding> appCommandBinding(UINT command) noexcept {
    for (const auto& binding : catalog) if (binding.command == command) {
        auto result = binding;
        if (result.route == Route::NamespaceAction) result.commandStore = namespaceActionCommand(result.action);
        return result;
    }
    return {};
}
std::wstring_view appCommandStoreName(UINT command) noexcept {
    const auto binding = appCommandBinding(command);
    return binding ? binding->commandStore : std::wstring_view{};
}

bool appCommandApplicable(const AppCommandBinding& binding,const AppCommandContext& context) noexcept {
    const unsigned flags = binding.requirements;
    if ((flags & RequireFolderView) && (!context.folderView || context.navigating)) return false;
    if ((flags & RequireSelection) && !context.selectionCount) return false;
    if ((flags & RequireSingleSelection) && context.selectionCount != 1) return false;
    if ((flags & RequireCopy) && !(context.selectionAttributes & SFGAO_CANCOPY)) return false;
    if ((flags & RequireMove) && !(context.selectionAttributes & SFGAO_CANMOVE)) return false;
    if ((flags & RequireDelete) && !(context.selectionAttributes & SFGAO_CANDELETE)) return false;
    if ((flags & RequireRename) && !(context.selectionAttributes & SFGAO_CANRENAME)) return false;
    if ((flags & RequirePhysicalDirectory) && !context.physicalDirectory) return false;
    if ((flags & RequireFilesystemSelection) && !(context.selectionAttributes & SFGAO_FILESYSTEM)) return false;
    if ((flags & RequireLibrary) && !context.library) return false;
    if ((flags & RequireWritableLibrary) && !context.writableLibrary) return false;
    if ((flags & RequireSearch) && !context.search) return false;
    if ((flags & RequireSearchBackground) && !context.searchBackground) return false;
    if ((flags & RequireDetailsView) && !context.detailsView) return false;
    if ((flags & RequireArchive) && !context.archive) return false;
    if ((flags & RequireComputer) && !context.computer) return false;
    if ((flags & RequireNetwork) && !context.network) return false;
    if ((flags & RequireDriveRoot) && (!context.driveRoot || context.selectionCount > 1)) return false;
    return true;
}

HRESULT queryAppCommand(NativeNamespaceActions& actions,UINT command,
                        const AppCommandContext& context,AppCommandCapability* result) {
    if (!result) return E_POINTER;
    const auto binding = appCommandBinding(command);
    if (!binding) return E_INVALIDARG;
    AppCommandCapability capability;
    capability.binding = *binding;
    if (!appCommandApplicable(*binding,context)) { *result = capability; return S_OK; }
    switch (binding->route) {
    case Route::Host:
        if (!binding->commandStore.empty() && !hasSeparateHostState(command)) {
            capability.status = actions.queryCommandState(binding->commandStore,&capability.native,binding->scope);
            if (capability.status == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
                if (const auto verb = staticVerb(command)) {
                    if(command!=RibbonRemoveProperties){capability.selectionVerb=verb;capability.selectionVerbs={verb};}
                    capability.status = actions.queryStaticVerbState(verb,&capability.native);
                }
                else if(ownsHostState(command)) {
                    capability.status=S_OK;capability.native.state=ECS_ENABLED;
                }
            }
        } else { capability.status = S_OK; capability.native.state = ECS_ENABLED; }
        break;
    case Route::NamespaceAction:
        capability.status=actions.queryActionState(binding->action,&capability.native,&capability.selectionVerbs);
        if(!capability.selectionVerbs.empty())capability.selectionVerb=capability.selectionVerbs.front();
        break;
    case Route::NativeZip: capability.status = actions.queryZipState(&capability.native); break;
    case Route::CommandStore:
        capability.status = actions.queryCommandState(binding->commandStore,&capability.native,binding->scope);
        if (capability.status == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
            if (command == Extract) capability.status = actions.queryExtractState(&capability.native);
            else if (const auto verb = staticVerb(command)) {
                if(command!=RibbonRemoveProperties){capability.selectionVerb=verb;capability.selectionVerbs={verb};}
                capability.status = actions.queryStaticVerbState(verb,&capability.native);
            }
            else capability.status = actions.queryRegisteredComponentState(binding->commandStore,&capability.native);
        }
        break;
    }
    capability.enabled = SUCCEEDED(capability.status) && capability.native.enabled();
    capability.checked = SUCCEEDED(capability.status) && capability.native.checked();
    *result = capability;
    return S_OK;
}

HRESULT invokeAppNativeCommand(NativeNamespaceActions& actions,UINT command,
                              const AppCommandContext& context,bool headless,POINT point) {
    if (headless) return E_ACCESSDENIED;
    const auto binding = appCommandBinding(command);
    if (!binding) return E_INVALIDARG;
    if (!appCommandApplicable(*binding,context)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    switch (binding->route) {
    case Route::NamespaceAction: return actions.invoke(binding->action,false,point);
    case Route::CommandStore:
        // Static wrappers can be enabled even without a selected association.
        // The native facade revalidates it after its visible-owner guard.
        return command == Extract ? actions.invokeExtract(false,point)
                                  : actions.invokeCommandStore(binding->commandStore,false,point,binding->scope,
                                       staticVerb(command) ? std::wstring_view(staticVerb(command)) : std::wstring_view{});
    case Route::NativeZip: return actions.invokeZip(false,point);
    case Route::Host: return E_NOTIMPL;
    }
    return E_UNEXPECTED;
}

HRESULT startAppCommandStateTask(NativeNamespaceActions& actions,UINT command,
                                 std::unique_ptr<NamespaceCommandStateTask>* result) {
    if(!result)return E_POINTER;
    const auto binding=appCommandBinding(command);if(!binding)return E_INVALIDARG;
    if(binding->commandStore.empty())return binding->route==Route::NamespaceAction
        ?actions.startActionStateTask(binding->action,result):HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if(binding->route==Route::NamespaceAction&&binding->action==Action::TroubleshootCompatibility&&
       (actions.facts().nativeAttributes&SFGAO_LINK))return actions.startRegisteredMenuStateTask(binding->commandStore,result);
    NamespaceCommandState native;
    const auto status=actions.queryCommandState(binding->commandStore,&native,binding->scope);
    if(status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)||status==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        if(binding->route==Route::NamespaceAction)return actions.startActionStateTask(binding->action,result);
        if(command==RibbonRemoveProperties)return actions.startRegisteredMenuStateTask(binding->commandStore,result);
        if(const auto verb=staticVerb(command))return actions.startStaticVerbStateTask(verb,result);
    }
    return actions.startCommandStateTask(binding->commandStore,result,binding->scope);
}

HRESULT startAppSelectionStateBatch(NativeNamespaceActions& actions,std::span<const AppCommandCapability> capabilities,
                                    std::unique_ptr<NamespaceCommandStateTask>* result,
                                    std::vector<AppSelectionStateBinding>* mapping) {
    if(!result||!mapping)return E_POINTER;
    if(capabilities.empty())return E_INVALIDARG;
    try {
        std::vector<AppSelectionStateBinding> bindings;bindings.reserve(capabilities.size());
        std::vector<std::wstring_view> verbs;
        for(const auto& capability:capabilities) {
            const auto binding=appCommandBinding(capability.binding.command);
            const auto expected=staticVerb(capability.binding.command);
            if(!binding||capability.status!=E_PENDING||capability.selectionVerb.empty()||capability.selectionVerbs.empty()||
               binding->scope!=Scope::Selection||capability.selectionVerb!=capability.selectionVerbs.front()||
               (binding->route!=Route::NamespaceAction&&(!expected||capability.selectionVerb!=expected||capability.selectionVerbs.size()!=1))||
               capability.binding.command==RibbonRemoveProperties)return E_INVALIDARG;
            for(const auto& previous:bindings)if(previous.command==binding->command)return E_INVALIDARG;
            AppSelectionStateBinding mapped;mapped.command=binding->command;mapped.verb=capability.selectionVerb;
            for(const auto verb:capability.selectionVerbs) {
                mapped.aliases.emplace_back(verb);
                if(std::find(verbs.begin(),verbs.end(),verb)==verbs.end())verbs.push_back(verb);
            }
            bindings.push_back(std::move(mapped));
        }
        std::unique_ptr<NamespaceCommandStateTask> task;
        const auto hr=actions.startStaticVerbStateBatch(verbs,&task);if(FAILED(hr))return hr;
        *mapping=std::move(bindings);*result=std::move(task);return S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
     catch(...){return E_FAIL;}
}

HRESULT applyAppSelectionStateBatch(const AppSelectionStateBinding& binding,
                                    std::span<const NamespaceSelectionVerbState> states,NamespaceCommandState* result) {
    if(!result)return E_POINTER;
    if(binding.aliases.empty())return E_INVALIDARG;
    const NamespaceCommandState* selectedState=nullptr;
    for(const auto& verb:binding.aliases) {
        const NamespaceSelectionVerbState* found=nullptr;
        for(const auto& entry:states)if(entry.verb==verb){if(found)return E_UNEXPECTED;found=&entry;}
        if(!found)return E_UNEXPECTED;
        if(found->status==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))continue;
        if(FAILED(found->status))return found->status;
        if(!selectedState||found->native.enabled())selectedState=&found->native;
        if(selectedState->enabled())break;
    }
    if(!selectedState)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    *result=*selectedState;return S_OK;
}

HRESULT queryAppCommandPopup(NativeNamespaceActions& actions,UINT command,
                             const AppCommandContext& context,NamespaceCommandPopup* result) {
    if (!result) return E_POINTER;
    const auto binding = appCommandBinding(command);
    if (!binding || binding->route != Route::CommandStore) return E_INVALIDARG;
    if (!appCommandApplicable(*binding,context)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    return actions.queryCommandStorePopup(binding->commandStore,result,binding->scope);
}

} // namespace explorer
