#pragma once
#include <windows.h>

namespace explorer {
enum Command : UINT {
    Back = 100, Forward, Up, Refresh, Address, Search, FileMenu, NewWindow, Close,
    Copy, Cut, Paste, CopyPath, CopyTo, MoveTo, Delete, PermanentDelete, Rename,
    NewFolder, NewText, Properties, Open, Edit, Pin, SelectAll, SelectNone, Invert,
    Print, Sharing, Security, FolderOptions, MapDrive, DisconnectDrive, Terminal,
    NavigationPane, PreviewPane, DetailsPane, HiddenItems, Extensions, Collapse,
    SortName, SortDate, SortType, SortSize, SortAscending, SortDescending,
    GroupNone, GroupName, GroupDate, GroupType, GroupSize, Checkboxes,
    QuickAccess, ThisPC, Desktop, Documents, Downloads, Pictures, Music, Videos,
    Network, RecycleBin, Libraries, HistoryMenu, ViewMenu, SortMenu, GroupMenu,
    Undo, Redo, PasteShortcut, NewShortcut, Zip, Extract, FileHistory, FocusSearch,
    FocusNext, FocusPrevious, Fullscreen, HideSelected, ColumnsMenu, SizeColumns,
    CloseSearch, SearchSubfolders, SearchCurrent, RecentSearches, SaveSearch,
    SearchKindMenu, SearchDateMenu, SearchSizeMenu, OpenFileLocation, NewItems,
    QuickAccessMenu, QuickAccessPlacement, QuickAccessReset,
    SharingProperties,
    NewLibrary, IncludeLibraryFolder, LibraryLocations, LibraryDefault, LibraryOptimize,
    ExpandAncestors, AddressList,
    ViewFirst = 500, ViewLast = ViewFirst + 7, BreadcrumbOverflow = 2999, BreadcrumbFirst = 3000
};
}
