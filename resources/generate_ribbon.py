"""Authoring helper only: emits committed XML; application/build needs no Python.

The Windows 10 command order follows the Microsoft Press File Explorer screenshots.
Microsoft UICC validates all control layouts during every CMake build.
"""
from pathlib import Path
import re
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
ids = {}
for file in ('commands.hpp', 'ribbon_commands.hpp'):
    text = (ROOT / 'include/explorer' / file).read_text()
    body = re.search(r'enum \w+ : UINT \{(.*?)\n\};', text, re.S).group(1)
    value = 0
    for entry in re.sub(r'//[^\n]*', '', body).split(','):
        entry = entry.strip()
        if not entry:
            continue
        parts = entry.split('=')
        name = parts[0].strip()
        if len(parts) == 2:
            expression = parts[1].strip()
            value = eval(expression, {'__builtins__': {}}, ids)
        ids[name] = value
        value += 1
for index in range(8):
    ids[f'View{index}'] = 500 + index

labels = {
    'RibbonNetworkTab': 'Network', 'RibbonShortcutTab': 'Manage', 'RibbonShortcutContext': 'Shortcut Tools',
    'RibbonAddNetworkDevice': 'Add devices and printers', 'RibbonDeviceWebpage': 'View device webpage',
    'RibbonConnectRemotePrinter': 'View remote printers', 'RibbonSearchActiveDirectory': 'Search Active Directory',
    'RibbonNetworkSharingCenter': 'Network and Sharing Center', 'RibbonShortcutOpenLocation': 'Open file location',
    'RibbonLibraryPublicSaveLocation': 'Set public save location', 'RibbonRemoveProperties': 'Remove properties',
    'RibbonDeleteConfirmation': 'Show recycle confirmation', 'RibbonRunAsAnotherUser': 'Run as different user',
    'RibbonExtractToGallery': 'Extract to', 'RibbonPinToTaskbar': 'Pin to taskbar',
    'RibbonClearSearchHistory': 'Clear search history',
    'RibbonAutoPlay': 'AutoPlay', 'RibbonFinishBurning': 'Finish burning', 'RibbonEraseDisc': 'Erase this disc',
    'RibbonOpenWith': 'Open with', 'RibbonFolderOptions': 'Options', 'RibbonBitLocker': 'BitLocker', 'RibbonMountDiscImage': 'Mount', 'RibbonBurnDiscImage': 'Burn',
    'RibbonCastToDevice': 'Cast to Device', 'RibbonWorkOffline': 'Work offline',
    'RibbonSyncOffline': 'Sync', 'RibbonSearchThisPC': 'This PC',
    'FileMenu': 'File', 'RibbonQuickAccess': 'Quick Access Toolbar',
    'RibbonHomeTab': 'Home', 'RibbonShareTab': 'Share', 'RibbonViewTab': 'View',
    'RibbonComputerTab': 'Computer', 'RibbonPictureTab': 'Manage', 'RibbonDriveTab': 'Manage',
    'RibbonCompressedTab': 'Extract', 'RibbonSearchTab': 'Search', 'RibbonLibraryTab': 'Manage',
    'RibbonRecycleTab': 'Manage', 'RibbonApplicationTab': 'Manage', 'RibbonMusicTab': 'Play',
    'RibbonVideoTab': 'Play', 'RibbonDiscImageTab': 'Disc Image Tools', 'RibbonDiscImageContext': 'Manage', 'RibbonPictureContext': 'Picture Tools', 'RibbonDriveContext': 'Drive Tools',
    'RibbonCompressedContext': 'Compressed Folder Tools', 'RibbonSearchContext': 'Search Tools',
    'RibbonLibraryContext': 'Library Tools', 'RibbonRecycleContext': 'Recycle Bin Tools',
    'RibbonApplicationContext': 'Application Tools', 'RibbonMusicContext': 'Music Tools',
    'RibbonVideoContext': 'Video Tools', 'RibbonFrequentPlaces': 'Frequent places',
    'RibbonLayoutGallery': 'Layout', 'RibbonShareGallery': 'Share with', 'RibbonNavigationMenu': 'Navigation pane',
    'RibbonEasyAccessMenu': 'Easy access', 'RibbonOptionsMenu': 'Options',
    'RibbonOpenMenu': 'Open', 'RibbonNewMenu': 'New item', 'RibbonDeleteMenu': 'Delete',
    'RibbonMapMenu': 'Map network drive', 'RibbonHelpMenu': 'Help',
    'RibbonPowerShellMenu': 'Open Windows PowerShell', 'RibbonHistoryMenu': 'History',
    'RibbonSearchAdvancedMenu': 'Advanced options', 'RibbonDateMenu': 'Date modified',
    'RibbonSizeMenu': 'Size', 'RibbonKindMenu': 'Kind',
    'RibbonLibraryOptimizeMenu': 'Optimize library for',
    'RibbonMoveMenu': 'Move to', 'RibbonCopyMenu': 'Copy to',
    'RibbonPropertiesMenu': 'Properties',
    'RibbonCopyToDesktop': 'Desktop', 'RibbonCopyToDocuments': 'Documents',
    'RibbonCopyToDownloads': 'Downloads', 'RibbonMoveToDesktop': 'Desktop',
    'RibbonMoveToDocuments': 'Documents', 'RibbonMoveToDownloads': 'Downloads',
    'RibbonLibraryChangeIcon': 'Change icon', 'RibbonLibraryShowInNavigation': 'Show in navigation pane',
    'RibbonSearchAgainMenu': 'Search again in',
    'Pin': 'Pin to Quick access', 'CopyPath': 'Copy path', 'PasteShortcut': 'Paste shortcut',
    'MoveTo': 'Move to', 'CopyTo': 'Copy to', 'PermanentDelete': 'Permanently delete',
    'NewFolder': 'New folder', 'NewText': 'Text document', 'NewShortcut': 'Shortcut',
    'NewItems': 'New item', 'FileHistory': 'History', 'SelectAll': 'Select all',
    'SelectNone': 'Select none', 'Invert': 'Invert selection', 'Sharing': 'Share', 'Zip': 'Zip',
    'Security': 'Advanced security', 'PreviewPane': 'Preview pane', 'DetailsPane': 'Details pane',
    'GroupMenu': 'Group by', 'SortMenu': 'Sort by', 'ColumnsMenu': 'Add columns',
    'SizeColumns': 'Size all columns to fit', 'Checkboxes': 'Item check boxes',
    'Extensions': 'File name extensions', 'HiddenItems': 'Hidden items',
    'HideSelected': 'Hide selected items', 'FolderOptions': 'Change folder and search options',
    'NewWindow': 'Open new window', 'Close': 'Close', 'Terminal': 'Open Windows PowerShell',
    'MapDrive': 'Map network drive', 'DisconnectDrive': 'Disconnect network drive',
    'SearchCurrent': 'Current folder', 'SearchSubfolders': 'All subfolders',
    'SearchKindMenu': 'Kind', 'SearchDateMenu': 'Date modified', 'SearchSizeMenu': 'Size',
    'RecentSearches': 'Recent searches', 'SaveSearch': 'Save search', 'CloseSearch': 'Close search', 'Extract': 'Extract all',
    'OpenFileLocation': 'Open file location', 'LibraryLocations': 'Manage library',
    'IncludeLibraryFolder': 'Include folder', 'LibraryDefault': 'Set save location',
    'LibraryOptimize': 'Optimize library for', 'NewLibrary': 'New library',
    'NavigationPane': 'Navigation pane', 'Collapse': 'Minimize the ribbon',
    'RibbonEmail': 'Email', 'RibbonBurnDisc': 'Burn to disc', 'RibbonFax': 'Fax',
    'RibbonSpecificPeople': 'Specific people...', 'RibbonStopSharing': 'Remove access',
    'RibbonAlwaysAvailableOffline': 'Always available offline',
    'RibbonIncludeInLibrary': 'Include in library', 'RibbonAddToFavorites': 'Add to favorites',
    'RibbonMapAsDrive': 'Map as drive', 'RibbonRotateLeft': 'Rotate left',
    'RibbonRotateRight': 'Rotate right', 'RibbonSlideShow': 'Slide show',
    'RibbonSetBackground': 'Set as background', 'RibbonOptimizeDrive': 'Optimize',
    'RibbonDiskCleanup': 'Cleanup', 'RibbonFormatDrive': 'Format', 'RibbonEjectDrive': 'Eject',
    'RibbonOpenSettings': 'Open Settings', 'RibbonSystemProperties': 'System properties',
    'RibbonUninstallProgram': 'Uninstall or change a program', 'RibbonManageComputer': 'Manage',
    'RibbonAddNetworkLocation': 'Add a network location', 'RibbonAccessMedia': 'Access media',
    'RibbonConnectRemote': 'Connect with Remote Desktop Connection',
    'RibbonRestoreAll': 'Restore all items', 'RibbonRestoreSelected': 'Restore the selected items',
    'RibbonEmptyRecycleBin': 'Empty Recycle Bin', 'RibbonRecycleProperties': 'Recycle Bin properties',
    'RibbonRunAsAdministrator': 'Run as administrator',
    'RibbonTroubleshootCompatibility': 'Troubleshoot compatibility', 'RibbonPinToStart': 'Pin to Start',
    'RibbonPlay': 'Play', 'RibbonPlayAll': 'Play all', 'RibbonAddToPlaylist': 'Add to playlist',
    'RibbonNewProcess': 'Open new window in new process',
    'RibbonPowerShellAdmin': 'Open Windows PowerShell as administrator',
    'RibbonHelp': 'Help', 'RibbonAbout': 'About Windows', 'RibbonResetLibrary': 'Restore settings',
    'RibbonRemoveLibraryFolder': 'Remove folder', 'RibbonSearchOtherProperties': 'Other properties',
    'RibbonSearchContents': 'File contents', 'RibbonSearchSystemFiles': 'System files',
    'RibbonSearchZipFiles': 'Zipped (compressed) folders',
    'RibbonChangeIndexedLocations': 'Change indexed locations',
    'RibbonExpandToCurrent': 'Expand to open folder', 'RibbonShowAllFolders': 'Show all folders',
    'RibbonShowLibraries': 'Show libraries', 'SortName': 'Name', 'SortDate': 'Date modified',
    'SortType': 'Type', 'SortSize': 'Size', 'SortAscending': 'Ascending',
    'SortDescending': 'Descending', 'GroupNone': '(None)', 'GroupName': 'Name',
    'GroupDate': 'Date modified', 'GroupType': 'Type', 'GroupSize': 'Size',
}
for i, label in enumerate(('Extra large icons', 'Large icons', 'Medium icons', 'Small icons', 'List', 'Details', 'Tiles', 'Content')):
    labels[f'View{i}'] = label
commands = {}
group_icons = []
keytips = {'FileMenu': 'F', 'RibbonHomeTab': 'H', 'RibbonShareTab': 'S', 'RibbonViewTab': 'V',
           'RibbonComputerTab': 'C', 'RibbonPictureTab': 'P', 'RibbonSearchTab': 'S',
           'Copy': 'CO', 'Cut': 'X', 'Paste': 'V', 'CopyPath': 'CP', 'PasteShortcut': 'PS',
           'Delete': 'D', 'Rename': 'R', 'NewFolder': 'N', 'Properties': 'PR', 'SelectAll': 'SA'}

def command(name, label=None):
    if name not in ids:
        ids[name] = 1500 + len([key for key in ids if key.startswith('Group')])
    commands[name] = (ids[name], label if label is not None else labels.get(name, re.sub(r'(?<!^)([A-Z])', r' \1', name)))
    return name

def button(name, kind='Button'):
    command(name)
    return f'<{kind} CommandName="{name}"/>'

def dropdown(name, children):
    command(name)
    return f'<DropDownButton CommandName="{name}">' + ''.join(children) + '</DropDownButton>'

def split(name, primary, children):
    command(name)
    return (f'<SplitButton CommandName="{name}"><SplitButton.ButtonItem>{button(primary)}</SplitButton.ButtonItem>'
            '<SplitButton.MenuGroups><MenuGroup>' + ''.join(children) + '</MenuGroup></SplitButton.MenuGroups></SplitButton>')

def gallery(name, in_ribbon=False):
    command(name)
    tag = 'InRibbonGallery' if in_ribbon else 'DropDownGallery'
    attributes = ' HasLargeItems="false" MaxRows="3" MaxColumns="2" MinColumnsLarge="2" TextPosition="Right"' if in_ribbon else ''
    gallery_type = 'Commands' if in_ribbon else 'Items'
    footer = button('RibbonClearSearchHistory') if name == 'RecentSearches' else ''
    return (f'<{tag} CommandName="{name}" Type="{gallery_type}"{attributes}>'
            f'<{tag}.MenuLayout><VerticalMenuLayout Rows="3"/></{tag}.MenuLayout>'
            f'<{tag}.MenuGroups><MenuGroup/>'+ (f'<MenuGroup>{footer}</MenuGroup>' if footer else '') + f'</{tag}.MenuGroups></{tag}>')

def share_gallery():
    command('RibbonShareGallery')
    command('RibbonSpecificPeople')
    return ('<InRibbonGallery CommandName="RibbonShareGallery" Type="Commands" HasLargeItems="false" '
            'MaxRows="1" MaxColumns="1" MinColumnsLarge="1" TextPosition="Right">'
            '<InRibbonGallery.MenuLayout><VerticalMenuLayout Rows="1"/></InRibbonGallery.MenuLayout>'
            '<InRibbonGallery.MenuGroups><MenuGroup/></InRibbonGallery.MenuGroups></InRibbonGallery>')

def extract_gallery():
    command('RibbonExtractToGallery')
    return ('<InRibbonGallery CommandName="RibbonExtractToGallery" Type="Items" HasLargeItems="false" '
            'ItemWidth="16" ItemHeight="16" MaxRows="3" MaxColumns="3" MinColumnsLarge="3" TextPosition="Right">'
            '<InRibbonGallery.MenuLayout><VerticalMenuLayout Rows="3"/></InRibbonGallery.MenuLayout>'
            '<InRibbonGallery.MenuGroups><MenuGroup/></InRibbonGallery.MenuGroups></InRibbonGallery>')

def group(name, label, controls, layout=None):
    command(name, label)
    first = re.search(r'CommandName="([^"]+)"', controls[0])
    if first:
        group_icons.append((ids[name], ids[first.group(1)]))
    if layout is None:
        templates = {1:'OneButton',2:'TwoButtons',3:'ThreeButtons',4:'FourButtons',5:'FiveButtons',6:'SixButtons',7:'SevenButtons',8:'EightButtons',9:'NineButtons'}
        definition = f' SizeDefinition="{templates[len(controls)]}"'
        body = ''
    else:
        definition = ''
        # Explicit large/medium layouts match Windows 10's prominent buttons
        # plus vertically stacked small commands instead of generic heuristics.
        body = '<SizeDefinition><ControlNameMap>' + ''.join(f'<ControlNameDefinition Name="c{i}"/>' for i in range(len(controls))) + '</ControlNameMap>'
        for size in ('Large', 'Medium', 'Small'):
            body += f'<GroupSizeDefinition Size="{size}">'
            columns = [[0],[1],[2],[3]] if name == 'GroupOrganize' and size == 'Large' else layout
            for column_index, column in enumerate(columns):
                if column_index:
                    body += '<ColumnBreak ShowSeparator="false"/>'
                keep_large = name in ('GroupClipboard','GroupNew','GroupOpen','GroupPanes','GroupSend','GroupCurrentView','GroupShowHide')
                if len(column) == 1 and (size == 'Large' or keep_large):
                    body += f'<ControlSizeDefinition ControlName="c{column[0]}" ImageSize="Large" IsLabelVisible="true"/>'
                else:
                    for i in column:
                        visible = not (size != 'Large' and name in ('GroupNew','GroupOpen','GroupCurrentView') and i != 0)
                        body += f'<Row><ControlSizeDefinition ControlName="c{i}" ImageSize="Small" IsLabelVisible="{str(visible).lower()}"/></Row>'
            body += '</GroupSizeDefinition>'
        body += '</SizeDefinition>'
    return name, f'<Group CommandName="{name}"{definition}>{body}{"".join(controls)}</Group>'

def tab(name, groups, mode=None):
    command(name)
    modes = f' ApplicationModes="{mode}"' if mode is not None else ''
    sizes = ''.join(f'<Scale Group="{g}" Size="Large"/>' for g, _ in groups)
    compact = ''.join(f'<Scale Group="{g}" Size="Small"/>' for g, xml in reversed(groups) if '<SizeDefinition>' in xml)
    popup = compact + ''.join(f'<Scale Group="{g}" Size="Popup"/>' for g, _ in reversed(groups))
    # Modal Groups do not inherit Tab modes; each needs explicit modes.
    contents=''.join(xml for _,xml in groups)
    if mode is not None: contents=contents.replace('<Group CommandName=',f'<Group ApplicationModes="{mode}" CommandName=')
    return (f'<Tab CommandName="{name}"{modes}><Tab.ScalingPolicy><ScalingPolicy><ScalingPolicy.IdealSizes>{sizes}'
            f'</ScalingPolicy.IdealSizes>{popup}</ScalingPolicy></Tab.ScalingPolicy>{contents}</Tab>')

home = tab('RibbonHomeTab', [
    group('GroupClipboard', 'Clipboard', [button('Pin'),button('Copy'),button('Paste'),button('Cut'),button('CopyPath'),button('PasteShortcut')], [[0],[1],[2],[3,4,5]]),
    group('GroupOrganize', 'Organize', [split('RibbonMoveMenu','MoveTo',[button('RibbonMoveToDesktop'),button('RibbonMoveToDocuments'),button('RibbonMoveToDownloads'),button('MoveTo')]),split('RibbonCopyMenu','CopyTo',[button('RibbonCopyToDesktop'),button('RibbonCopyToDocuments'),button('RibbonCopyToDownloads'),button('CopyTo')]),split('RibbonDeleteMenu','Delete',[button('Delete'),button('PermanentDelete'),button('RibbonDeleteConfirmation','CheckBox')]),button('Rename')], [[0,1],[2,3]]),
    group('GroupNew', 'New', [button('NewFolder'),gallery('RibbonNewMenu'),dropdown('RibbonEasyAccessMenu',[button('Pin'),button('RibbonIncludeInLibrary'),button('RibbonAddToFavorites'),button('RibbonMapAsDrive'),button('RibbonAlwaysAvailableOffline'),button('RibbonWorkOffline'),button('RibbonSyncOffline'),button('Properties')])], [[0],[1,2]]),
    group('GroupOpen', 'Open', [split('RibbonPropertiesMenu','Properties',[button('Properties'),button('RibbonRemoveProperties')]),split('RibbonOpenMenu','Open',[button('Open'),button('RibbonOpenWith'),button('Edit'),button('Print')]),button('Edit'),button('FileHistory')], [[0],[1,2,3]]),
    group('GroupSelect', 'Select', [button('SelectAll'),button('SelectNone'),button('Invert')], [[0,1,2]]),
], '0,2')
share = tab('RibbonShareTab', [
    group('GroupSend', 'Send', [button('Sharing'),button('RibbonEmail'),button('Zip'),button('RibbonBurnDisc'),button('Print'),button('RibbonFax')], [[0],[1],[2],[3,4,5]]),
    group('GroupShareWith', 'Share with', [share_gallery(),button('RibbonStopSharing'),button('Security')], [[0],[1],[2]]),
], '0,3')
nav = dropdown('RibbonNavigationMenu',[button('NavigationPane','CheckBox'),button('RibbonExpandToCurrent','CheckBox'),button('RibbonShowAllFolders','CheckBox'),button('RibbonShowLibraries','CheckBox')])
view = tab('RibbonViewTab', [
    group('GroupPanes', 'Panes', [nav,button('PreviewPane','ToggleButton'),button('DetailsPane','ToggleButton')], [[0],[1,2]]),
    ('GroupLayout', '<Group CommandName="GroupLayout" SizeDefinition="OneInRibbonGallery">'+gallery('RibbonLayoutGallery',True)+'</Group>'),
    group('GroupCurrentView', 'Current view', [dropdown('SortMenu',[button('SortName','ToggleButton'),button('SortDate','ToggleButton'),button('SortType','ToggleButton'),button('SortSize','ToggleButton'),button('SortAscending','ToggleButton'),button('SortDescending','ToggleButton')]),dropdown('GroupMenu',[button('GroupNone','ToggleButton'),button('GroupName','ToggleButton'),button('GroupDate','ToggleButton'),button('GroupType','ToggleButton'),button('GroupSize','ToggleButton')]),button('ColumnsMenu'),button('SizeColumns')], [[0],[1,2,3]]),
    group('GroupShowHide', 'Show/hide', [button('Checkboxes','CheckBox'),button('Extensions','CheckBox'),button('HiddenItems','CheckBox'),button('HideSelected')], [[0,1,2],[3]]),
    group('GroupOptions', '', [split('RibbonOptionsMenu','RibbonFolderOptions',[button('FolderOptions')])]),
], '0,1,2,4')
command('GroupLayout', 'Layout')
for i in range(8): command(f'View{i}')
computer = tab('RibbonComputerTab',[
    group('GroupComputerLocation','Location',[button('Properties'),button('Open'),button('Rename')],[[0],[1],[2]]),
    group('GroupNetwork','Network',[button('RibbonAccessMedia'),dropdown('RibbonMapMenu',[button('MapDrive'),button('DisconnectDrive')]),button('RibbonAddNetworkLocation')]),
    group('GroupSystem','System',[button('RibbonOpenSettings'),button('RibbonUninstallProgram'),button('RibbonSystemProperties'),button('RibbonManageComputer')],[[0],[1,2,3]]),
], '1,5')

contexts = []
def contextual(context, name, groups):
    command(context)
    contexts.append(f'<TabGroup CommandName="{context}">{tab(name,groups,"0,1,2")}</TabGroup>')

contextual('RibbonPictureContext','RibbonPictureTab',[
    group('GroupPictureRotate','Rotate',[button('RibbonRotateLeft'),button('RibbonRotateRight')]),
    group('GroupPictureView','View',[button('RibbonSlideShow'),button('RibbonSetBackground')]),
])
contextual('RibbonDriveContext','RibbonDriveTab',[
    group('GroupDriveProtection','Protect',[button('RibbonBitLocker')]),
    group('GroupDriveManage','Manage',[button('RibbonOptimizeDrive'),button('RibbonDiskCleanup'),button('RibbonFormatDrive')]),
    group('GroupDriveMedia','Media',[button('RibbonAutoPlay'),button('RibbonEjectDrive'),button('RibbonFinishBurning'),button('RibbonEraseDisc')],[[0],[1],[2,3]]),
])
contextual('RibbonCompressedContext','RibbonCompressedTab',[
    ('GroupExtractTo','<Group CommandName="GroupExtractTo" SizeDefinition="OneInRibbonGallery">'+extract_gallery()+'</Group>'),
    group('GroupExtract','',[button('Extract')]),
])
command('GroupExtractTo','Extract to')
contextual('RibbonSearchContext','RibbonSearchTab',[
    group('GroupSearchLocation','Location',[button('RibbonSearchThisPC'),button('SearchCurrent','ToggleButton'),button('SearchSubfolders','ToggleButton'),dropdown('RibbonSearchAgainMenu',[button('RibbonSearchThisPC'),button('RibbonChangeIndexedLocations')])],[[0],[1,2,3]]),
    group('GroupSearchRefine','Refine',[gallery('SearchDateMenu'),gallery('SearchKindMenu'),gallery('SearchSizeMenu'),gallery('RibbonSearchOtherProperties')],[[0],[1,2,3]]),
    group('GroupSearchOptions','Options',[gallery('RecentSearches'),dropdown('RibbonSearchAdvancedMenu',[button('RibbonChangeIndexedLocations'),button('RibbonSearchContents','CheckBox'),button('RibbonSearchSystemFiles','CheckBox'),button('RibbonSearchZipFiles','CheckBox')]),button('SaveSearch'),button('OpenFileLocation')],[[0,1,2],[3]]),
    group('GroupSearchClose','Close',[button('CloseSearch')]),
])
contextual('RibbonLibraryContext','RibbonLibraryTab',[
    group('GroupLibraryManage','Manage',[button('LibraryLocations'),button('LibraryDefault'),button('LibraryOptimize'),button('RibbonLibraryChangeIcon'),button('RibbonLibraryShowInNavigation','CheckBox')],[[0],[1],[2,3,4]]),
    group('GroupLibraryRestore','',[button('RibbonResetLibrary')]),
])
contextual('RibbonRecycleContext','RibbonRecycleTab',[
    group('GroupRecycleManage','Manage',[button('RibbonEmptyRecycleBin'),button('RibbonRecycleProperties')]),
    group('GroupRecycleRestore','Restore',[button('RibbonRestoreAll'),button('RibbonRestoreSelected')]),
])
contextual('RibbonApplicationContext','RibbonApplicationTab',[
    group('GroupApplication','Application',[button('RibbonPinToTaskbar'),button('RibbonRunAsAdministrator'),button('RibbonTroubleshootCompatibility')]),
])
contextual('RibbonMusicContext','RibbonMusicTab',[
    group('GroupMusicPlay','Play',[button('RibbonPlay'),button('RibbonPlayAll'),button('RibbonCastToDevice'),button('RibbonAddToPlaylist')]),
])
contextual('RibbonVideoContext','RibbonVideoTab',[
    group('GroupVideoPlay','Play',[button('RibbonPlay'),button('RibbonPlayAll'),button('RibbonCastToDevice'),button('RibbonAddToPlaylist')]),
])
contextual('RibbonDiscImageContext','RibbonDiscImageTab',[
    group('GroupDiscImage','Manage',[button('RibbonMountDiscImage'),button('RibbonBurnDiscImage')]),
])
network = tab('RibbonNetworkTab',[
    group('GroupNetworkLocation','Location',[button('Properties'),button('Open'),button('RibbonConnectRemote')],[[0],[1],[2]]),
    group('GroupNetworkDevices','Network',[button('RibbonAddNetworkDevice'),button('RibbonDeviceWebpage'),button('RibbonConnectRemotePrinter'),button('RibbonSearchActiveDirectory')],[[0],[1,2,3]]),
    group('GroupNetworkSharing','',[button('RibbonNetworkSharingCenter')]),
], '2')
contextual('RibbonShortcutContext','RibbonShortcutTab',[
    group('GroupShortcut','Open',[button('RibbonShortcutOpenLocation')]),
])
command('FileMenu');command('RibbonFrequentPlaces');command('RibbonQuickAccess');command('RibbonHelp');command('RibbonHelpButton','Help')
qat = ''.join(button(n).replace('/>',f' ApplicationDefaults.IsChecked="{str(n in ("Properties","NewFolder")).lower()}"/>') for n in ('Undo','Redo','Delete','Properties','NewFolder','Rename'))
def file_mode(control):
    # Only left-side top-level menu controls are modal. Nested menu entries
    # inherit their owner's availability and UICC rejects modal attributes.
    return re.sub(r'^<(Button|SplitButton|DropDownButton) ',r'<\1 ApplicationModes="0,1,2" ',control,count=1)
filemenu = ''.join(file_mode(control) for control in (
            split('RibbonNewMenu','NewWindow',[button('NewWindow'),button('RibbonNewProcess')]),
            split('RibbonPowerShellMenu','Terminal',[button('Terminal'),button('RibbonPowerShellAdmin')]),
            button('FolderOptions'), dropdown('RibbonHelpMenu',[button('RibbonHelp'),button('RibbonAbout')]),button('Close')))
# A split-button command cannot have two different child layouts in one view;
# the File menu uses a separate named command for its New Window split button.
filemenu = filemenu.replace('CommandName="RibbonNewMenu"','CommandName="RibbonNewWindowMenu"')
ids['RibbonNewWindowMenu']=1250;command('RibbonNewWindowMenu','Open new window')
# Every persisted command can be added to the native QAT, even before its page
# is materialized. The framework creates its handler lazily from this catalog.
for name, identifier in list(ids.items()):
    if 100 <= identifier < 500 and name not in commands:
        command(name)

declarations = []
for name,(identifier,label) in commands.items():
    tip = keytips.get(name,'')
    attrs = f'Name="{name}" Id="{identifier}"'
    if label: attrs += f' LabelTitle="{escape(label, {chr(34): "&quot;"})}"'
    if tip: attrs += f' Keytip="{tip}"'
    if label: attrs += f' TooltipTitle="{escape(label, {chr(34): "&quot;"})}"'
    declarations.append(f'<Command {attrs}/>')
views = ('<Ribbon Name="Windows10Explorer" GroupSpacing="Small">'
         '<Ribbon.ApplicationMenu><ApplicationMenu CommandName="FileMenu">'
         '<ApplicationMenu.RecentItems><RecentItems CommandName="RibbonFrequentPlaces" MaxCount="10" EnablePinning="true"/></ApplicationMenu.RecentItems>'
         '<MenuGroup>'+filemenu+'</MenuGroup></ApplicationMenu></Ribbon.ApplicationMenu>'
         '<Ribbon.QuickAccessToolbar><QuickAccessToolbar CommandName="RibbonQuickAccess"><QuickAccessToolbar.ApplicationDefaults>'+qat+
         '</QuickAccessToolbar.ApplicationDefaults></QuickAccessToolbar></Ribbon.QuickAccessToolbar>'
         '<Ribbon.HelpButton><HelpButton CommandName="RibbonHelpButton"/></Ribbon.HelpButton>'
         '<Ribbon.Tabs>'+home+share+computer+network+view+'</Ribbon.Tabs>'
         '<Ribbon.ContextualTabs>'+''.join(sorted(contexts, key=lambda value: ('RibbonDriveContext','RibbonSearchContext','RibbonLibraryContext','RibbonPictureContext','RibbonCompressedContext','RibbonRecycleContext','RibbonApplicationContext','RibbonMusicContext','RibbonVideoContext','RibbonDiscImageContext','RibbonShortcutContext').index(re.search(r'CommandName="([^"]+)"',value).group(1))))+'</Ribbon.ContextualTabs></Ribbon>')
output = ('<?xml version="1.0" encoding="utf-8"?>\n'
          '<Application xmlns="http://schemas.microsoft.com/windows/2009/Ribbon">\n'
          '<Application.Commands>\n'+'\n'.join(declarations)+'\n</Application.Commands>\n'
          '<Application.Views>'+views+'</Application.Views>\n</Application>\n')
(ROOT/'resources/ribbon.xml').write_text(output, encoding='utf-8')
(ROOT/'resources/ribbon_group_icons.inc').write_text('\n'.join(f'{{{group}, {icon}}},' for group,icon in group_icons)+'\n', encoding='utf-8')

import json
(ROOT/'resources/ribbon_labels.inc').write_text('\n'.join(
    f'{{{identifier}, L{json.dumps(label, ensure_ascii=True)}}},'
    for _, (identifier, label) in commands.items())+'\n', encoding='utf-8')
