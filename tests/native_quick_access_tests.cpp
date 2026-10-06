#include "explorer/ribbon.hpp"
#include "explorer/commands.hpp"
#include "explorer/headless_visual.hpp"
#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <propsys.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <ocidl.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
unsigned assertions = 0;
constexpr std::array<UINT, 21> knownCommands{explorer::Copy, explorer::Cut, explorer::Paste, explorer::CopyPath,
    explorer::PasteShortcut, explorer::Rename, explorer::NewFolder, explorer::Properties, explorer::Open, explorer::Edit,
    explorer::Print, explorer::Undo, explorer::SelectAll, explorer::SelectNone, explorer::Invert, explorer::PreviewPane,
    explorer::DetailsPane, explorer::HiddenItems, explorer::Extensions, explorer::Checkboxes, explorer::FileHistory};
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
void exact(HRESULT result, const char* message) {
    ++assertions;
    if (result != S_OK) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<ULONG>(result)));
}
struct Variant { PROPVARIANT value{}; ~Variant() { PropVariantClear(&value); } };
struct Window {
    HWND handle = nullptr;
    ~Window() {
        if (handle && !DestroyWindow(handle)) {
            std::cerr << "FAIL owned QAT HWND cleanup error=" << GetLastError() << std::endl;
            std::_Exit(9);
        }
    }
};
struct Directory {
    fs::path path;
    HANDLE lease = INVALID_HANDLE_VALUE;
    FILE_ID_INFO identity{};
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    Directory() {
        GUID id{}; exact(CoCreateGuid(&id), "Create unique owned settings name");
        std::array<wchar_t, 40> name{};
        require(StringFromGUID2(id, name.data(), static_cast<int>(name.size())) != 0, "Format owned settings name");
        path = fs::temp_directory_path() / (L"WindowsExplorer-QAT-Native-" + std::wstring(name.data()));
        require(path.is_absolute(), "Owned settings directory must be absolute");
        require(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "Create new owned settings directory");
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            RemoveDirectoryW(path.c_str());
            throw std::runtime_error("New owned settings directory attributes could not be verified");
        }
        // Deny directory replacement while native settings and row aliases live.
        lease = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lease == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(lease, FileIdInfo, &identity, sizeof(identity))) {
            if (lease != INVALID_HANDLE_VALUE) CloseHandle(lease);
            lease = INVALID_HANDLE_VALUE; RemoveDirectoryW(path.c_str());
            throw std::runtime_error("Retain original owned settings directory FileID");
        }
    }
    bool current() const {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (!path.is_absolute() || attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const HANDLE actual = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (actual == INVALID_HANDLE_VALUE) return false;
        FILE_ID_INFO observed{};
        const bool read = GetFileInformationByHandleEx(actual, FileIdInfo, &observed, sizeof(observed)) != FALSE;
        const bool closed = CloseHandle(actual) != FALSE;
        return read && closed && observed.VolumeSerialNumber == identity.VolumeSerialNumber &&
            std::equal(std::begin(observed.FileId.Identifier), std::end(observed.FileId.Identifier), std::begin(identity.FileId.Identifier));
    }
    ~Directory() {
        try {
            require(current(), "Owned settings directory FileID changed before cleanup");
            std::error_code error; fs::directory_iterator entry(path, error), end;
            require(!error, "Enumerate owned settings cleanup");
            constexpr std::array names{L"authored-twenty.bin", L"authored-above.bin", L"authored-below.bin",
                L"installed-twenty.bin", L"installed-above.bin", L"installed-below.bin",
                L"authored-envelope.bin", L"authored-invalid.bin", L"authored-legacy.bin",
                L"installed-envelope.bin", L"installed-invalid.bin", L"installed-legacy.bin"};
            unsigned count = 0;
            while (entry != end) {
                const auto file = entry->path(); const DWORD flags = GetFileAttributesW(file.c_str());
                const auto name = file.filename().wstring();
                require(++count <= names.size() && file.parent_path() == path && flags != INVALID_FILE_ATTRIBUTES &&
                    !(flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "Unexpected owned settings cleanup entry");
                require(std::any_of(names.begin(), names.end(), [&](const auto* allowed) { return name == allowed; }),
                    "Cleanup refuses an unowned settings filename");
                require(current(), "Owned settings directory FileID changed before file deletion");
                require(DeleteFileW(file.c_str()) != FALSE, "Delete only an owned settings file");
                entry.increment(error); require(!error, "Continue owned settings cleanup");
            }
            require(current(), "Owned settings directory FileID changed before final deletion");
            require(CloseHandle(lease) != FALSE, "Release owned directory replacement lease"); lease = INVALID_HANDLE_VALUE;
            require(RemoveDirectoryW(path.c_str()) != FALSE, "Remove exact empty owned settings directory");
        } catch (...) { std::cerr << "FAIL owned QAT settings cleanup" << std::endl; std::_Exit(9); }
    }
};
void isolation(HWND owner, ULONGLONG deadline) {
    require(GetTickCount64() < deadline, "QAT native fixture exceeded its admission deadline");
    const auto* desktop = explorer::PrivateDesktop::current();
    require(desktop != nullptr, "QAT native fixture lacks private desktop");
    exact(desktop->verifyIsolation(), "QAT native desktop identity");
    DWORD process = 0;
    require(IsWindow(owner) && GetWindowThreadProcessId(owner, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId(), "QAT owner must remain on its original process STA");
    bool visible = true; exact(desktop->visibleWindowsOnInputDesktop(visible), "QAT input desktop observation");
    require(!visible, "QAT test exposed a visible window on the input desktop");
}
void materialize(explorer::NativeRibbon& ribbon, HWND owner, ULONGLONG deadline) {
    exact(ribbon.flush(), "Flush actual native Ribbon properties");
    MSG message{};
    unsigned count = 0;
    while (count < 64 && GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        require(message.message != WM_QUIT, "Unexpected QAT host quit");
        TranslateMessage(&message); DispatchMessageW(&message); ++count;
    }
    isolation(owner, deadline);
}

struct Row {
    ComPtr<IUnknown> object, identity;
    HRESULT query = E_PENDING, commandRead = E_PENDING, commandConvert = E_PENDING;
    VARTYPE commandVariant = VT_EMPTY;
    UINT command = 0;
    HRESULT typeRead = E_PENDING, typeConvert = E_PENDING;
    VARTYPE typeVariant = VT_EMPTY;
    UINT type = UI_COMMANDTYPE_UNKNOWN;
    HRESULT labelRead = E_PENDING;
    VARTYPE labelVariant = VT_EMPTY;
    std::wstring label;
    HRESULT enabledRead = E_PENDING;
    VARTYPE enabledVariant = VT_EMPTY;
    VARIANT_BOOL enabled = VARIANT_FALSE;
};
struct NativeState {
    ComPtr<IUICollection> collection;
    ComPtr<IUnknown> collectionIdentity;
    ComPtr<IPropertyStore> view;
    UINT dock = UI_CONTROLDOCK_TOP;
    std::vector<Row> rows;
};
NativeState readNative(explorer::NativeRibbon& ribbon, HWND owner, ULONGLONG deadline, const char* phase) {
    isolation(owner, deadline);
    auto* const framework = ribbon.nativeFramework();
    require(framework != nullptr, "No real native Ribbon framework");
    Variant source;
    exact(framework->GetUICommandProperty(ribbon.nativeCommandId(explorer::RibbonQuickAccess), UI_PKEY_ItemsSource,
        &source.value), "Read actual native QAT ItemsSource");
    require(source.value.vt == VT_UNKNOWN && source.value.punkVal, "Native QAT ItemsSource is not an object");
    NativeState state;
    exact(source.value.punkVal->QueryInterface(IID_PPV_ARGS(&state.collection)), "Read actual IUICollection");
    exact(state.collection.As(&state.collectionIdentity), "Read native collection canonical identity");
    ComPtr<IUIRibbon> view; exact(framework->GetView(0, IID_PPV_ARGS(&view)), "Read actual native Ribbon view");
    exact(view.As(&state.view), "Read native Ribbon view property store");
    Variant dock; exact(state.view->GetValue(UI_PKEY_QuickAccessToolbarDock, &dock.value), "Read actual native dock");
    ULONG nativeDock = 0; exact(PropVariantToUInt32(dock.value, &nativeDock), "Convert actual native dock");
    require(nativeDock == UI_CONTROLDOCK_TOP || nativeDock == UI_CONTROLDOCK_BOTTOM, "Unsupported native dock value");
    state.dock = nativeDock;
    UINT count = 0; exact(state.collection->GetCount(&count), "Read complete actual native QAT count");
    require(count <= 20, "Native QAT count exceeds documented capacity");
    state.rows.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        Row row; exact(state.collection->GetItem(index, &row.object), "Read native QAT row object");
        require(row.object != nullptr, "Actual QAT row is null");
        exact(row.object.As(&row.identity), "Read canonical native row identity");
        ComPtr<IUISimplePropertySet> properties; row.query = row.object.As(&properties);
        if (row.query == S_OK && properties) {
            Variant command; row.commandRead = properties->GetValue(UI_PKEY_CommandId, &command.value);
            row.commandVariant = command.value.vt;
            if (row.commandRead == S_OK) {
                ULONG id = 0; row.commandConvert = PropVariantToUInt32(command.value, &id);
                if (row.commandConvert == S_OK) row.command = id;
            }
            Variant type; row.typeRead = properties->GetValue(UI_PKEY_CommandType, &type.value);
            row.typeVariant = type.value.vt;
            if (row.typeRead == S_OK) { ULONG value = 0; row.typeConvert = PropVariantToUInt32(type.value, &value); if (row.typeConvert == S_OK) row.type = value; }
            Variant label; row.labelRead = properties->GetValue(UI_PKEY_Label, &label.value);
            row.labelVariant = label.value.vt;
            if (row.labelRead == S_OK && label.value.vt == VT_LPWSTR && label.value.pwszVal) row.label = label.value.pwszVal;
            else if (row.labelRead == S_OK && label.value.vt == VT_BSTR && label.value.bstrVal) row.label = label.value.bstrVal;
        }
        if (row.commandRead == S_OK && row.commandConvert == S_OK && row.command) {
            Variant enabled; row.enabledRead = framework->GetUICommandProperty(row.command, UI_PKEY_Enabled, &enabled.value);
            row.enabledVariant = enabled.value.vt;
            if (row.enabledRead == S_OK && enabled.value.vt == VT_BOOL) row.enabled = enabled.value.boolVal;
        }
        std::cout << "QAT native phase=" << phase << " index=" << index << " identity=" << row.identity.Get()
            << " query=" << static_cast<ULONG>(row.query) << " commandRead=" << static_cast<ULONG>(row.commandRead)
            << " commandVT=" << row.commandVariant << " commandConvert=" << static_cast<ULONG>(row.commandConvert)
            << " nativeId=" << row.command << " typeRead=" << static_cast<ULONG>(row.typeRead) << " type=" << row.type
            << " enabledRead=" << static_cast<ULONG>(row.enabledRead) << " enabledVT=" << row.enabledVariant
            << " enabledRaw=" << row.enabled << std::endl;
        state.rows.push_back(std::move(row));
    }
    UINT finalCount = 0; exact(state.collection->GetCount(&finalCount), "Recheck actual native QAT count");
    require(finalCount == count, "Native QAT count changed during readback");
    for (UINT index = 0; index < finalCount; ++index) {
        ComPtr<IUnknown> row, identity; exact(state.collection->GetItem(index, &row), "Recheck native QAT row");
        exact(row.As(&identity), "Recheck native QAT row canonical identity");
        require(identity.Get() == state.rows[index].identity.Get(), "Native QAT rows changed during readback");
    }
    Variant finalDock;
    exact(state.view->GetValue(UI_PKEY_QuickAccessToolbarDock, &finalDock.value), "Recheck actual native dock");
    ULONG finalPlacement = 0; exact(PropVariantToUInt32(finalDock.value, &finalPlacement), "Convert final actual dock");
    require(finalPlacement == state.dock, "Native dock changed during readback");
    isolation(owner, deadline);
    return state;
}
bool sameRow(const Row& left, const Row& right) {
    return left.identity.Get() == right.identity.Get() && left.query == right.query &&
        left.commandRead == right.commandRead && left.commandConvert == right.commandConvert &&
        left.commandVariant == right.commandVariant && left.command == right.command &&
        left.typeRead == right.typeRead && left.typeConvert == right.typeConvert &&
        left.typeVariant == right.typeVariant && left.type == right.type &&
        left.labelRead == right.labelRead && left.labelVariant == right.labelVariant && left.label == right.label &&
        left.enabledRead == right.enabledRead && left.enabledVariant == right.enabledVariant && left.enabled == right.enabled;
}
void exactState(const NativeState& expected, const NativeState& actual) {
    require(expected.collectionIdentity.Get() == actual.collectionIdentity.Get() && expected.dock == actual.dock &&
        expected.rows.size() == actual.rows.size(), "Native collection identity/dock/count changed unexpectedly");
    for (size_t index = 0; index < expected.rows.size(); ++index)
        require(sameRow(expected.rows[index], actual.rows[index]), "An untouched actual native row changed identity or metadata");
}
void ids(const NativeState& state, explorer::NativeRibbon& ribbon, std::span<const UINT> expected) {
    require(state.rows.size() == expected.size(), "Native command list is incomplete");
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& row = state.rows[index];
        require(row.query == S_OK && row.commandRead == S_OK && row.commandConvert == S_OK && row.command != 0 &&
            row.command == ribbon.nativeCommandId(expected[index]), "Actual native command order differs");
        require(row.enabledRead == S_OK && row.enabledVariant == VT_BOOL, "Declared QAT command has no actual enabled property");
        require((row.enabled != VARIANT_FALSE) == (expected[index] != explorer::Copy), "Native QAT command state was replaced");
    }
}
explorer::RibbonQuickAccessSnapshot snapshot(explorer::NativeRibbon& ribbon, const NativeState& actual) {
    explorer::RibbonQuickAccessSnapshot result; exact(ribbon.quickAccessSnapshot(result), "Capture actual native snapshot");
    require(result.items().size() == actual.rows.size() && result.belowRibbon() == (actual.dock == UI_CONTROLDOCK_BOTTOM),
        "Public snapshot differs from independent actual count/dock");
    unsigned unreadable = 0, unmapped = 0;
    for (size_t index = 0; index < actual.rows.size(); ++index) {
        const auto& row = actual.rows[index]; const auto& item = result.items()[index];
        const HRESULT expected = row.query != S_OK ? row.query : row.commandRead != S_OK ? row.commandRead : row.commandConvert;
        require(item.commandRead == expected && (expected != S_OK || item.nativeCommand == row.command),
            "Public snapshot masks an actual native command readback");
        if (expected == S_OK) {
            const auto mapped = std::find_if(knownCommands.begin(), knownCommands.end(),
                [&](UINT command) { return ribbon.nativeCommandId(command) == row.command; });
            if (mapped != knownCommands.end()) require(item.command == *mapped, "Public snapshot mapped a known native ID incorrectly");
        }
        if (expected != S_OK) ++unreadable;
        else if (!item.command) ++unmapped;
    }
    std::cout << "QAT observed opaqueReadFailures=" << unreadable << " readableUnmapped=" << unmapped
        << " opaqueSeparatorCoverage=NOT_COVERED(no demonstrated native separator)" << std::endl;
    return result;
}
void dock(const NativeState& state, bool below) {
    Variant value; exact(InitPropVariantFromUInt32(below ? UI_CONTROLDOCK_BOTTOM : UI_CONTROLDOCK_TOP, &value.value), "Native dock value");
    exact(state.view->SetValue(UI_PKEY_QuickAccessToolbarDock, value.value), "Set actual native dock");
    exact(state.view->Commit(), "Commit actual native dock");
}
void edit(explorer::NativeRibbon& ribbon, const explorer::RibbonQuickAccessSnapshot& captured,
          explorer::RibbonQuickAccessEditKind kind, UINT command, UINT index, UINT destination) {
    exact(ribbon.editQuickAccess(captured, {kind, command, index, destination}), "One targeted actual native QAT edit");
}

void runLayout(explorer::RibbonLayout layout, const Directory& files, ULONGLONG deadline) {
    using namespace explorer;
    const char* label = layout == RibbonLayout::Authored ? "authored" : "installed";
    Window window{CreateWindowExW(0, L"WindowsExplorerNativeQATRegression", L"Owned native QAT regression", WS_OVERLAPPEDWINDOW,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    require(window.handle != nullptr, "Create hidden private QAT owner");
    unsigned executions = 0; NativeRibbon ribbon;
    RibbonCallbacks callbacks;
    callbacks.execute = [&](UINT) { ++executions; return E_ACCESSDENIED; };
    callbacks.executeItem = [&](UINT, UINT) { ++executions; return E_ACCESSDENIED; };
    callbacks.query = [](UINT command) { RibbonCommandState state; state.enabled = command != Copy; return state; };
    exact(ribbon.initialize(window.handle, GetModuleHandleW(nullptr), std::move(callbacks), layout), "Initialize real native QAT framework");
    require(ribbon.layout() == layout && (layout != RibbonLayout::InstalledWindows10 || ribbon.installedLayoutStatus() == S_OK),
        "Requested installed Ribbon is unsupported; fallback is not native coverage");
    materialize(ribbon, window.handle, deadline);
    auto initial = readNative(ribbon, window.handle, deadline, "initial");
    const std::array<UINT, 2> defaults{Properties, NewFolder}; ids(initial, ribbon, defaults);
    auto captured = snapshot(ribbon, initial);
    edit(ribbon, captured, RibbonQuickAccessEditKind::Add, Copy, 0, 0);
    materialize(ribbon, window.handle, deadline);
    auto added = readNative(ribbon, window.handle, deadline, "targeted-add");
    const std::array<UINT, 3> three{Properties, NewFolder, Copy}; ids(added, ribbon, three);
    require(added.rows.size() == initial.rows.size() + 1, "Add did not add exactly one row");
    for (size_t index = 0; index < initial.rows.size(); ++index) require(sameRow(initial.rows[index], added.rows[index]), "Add rebuilt an existing native row");
    auto addedSnapshot = snapshot(ribbon, added);
    require(ribbon.editQuickAccess(addedSnapshot, {RibbonQuickAccessEditKind::Add, Copy, 0, 0}) == S_FALSE, "Duplicate Add did not preserve native list");
    exactState(added, readNative(ribbon, window.handle, deadline, "duplicate-preserved"));
    edit(ribbon, addedSnapshot, RibbonQuickAccessEditKind::Remove, 0, 2, 0);
    exactState(initial, readNative(ribbon, window.handle, deadline, "targeted-remove"));
    auto moveSnapshot = snapshot(ribbon, initial);
    edit(ribbon, moveSnapshot, RibbonQuickAccessEditKind::Move, 0, 1, 0);
    auto moved = readNative(ribbon, window.handle, deadline, "targeted-move");
    auto reversed = initial; std::reverse(reversed.rows.begin(), reversed.rows.end()); exactState(reversed, moved);

    auto staleDock = snapshot(ribbon, moved); dock(moved, true);
    auto changedDock = readNative(ribbon, window.handle, deadline, "native-dock-mutation");
    require(ribbon.editQuickAccess(staleDock, {RibbonQuickAccessEditKind::Move, 0, 0, 1}) == HRESULT_FROM_WIN32(ERROR_RETRY), "Stale dock edit overwrote native state");
    exactState(changedDock, readNative(ribbon, window.handle, deadline, "stale-dock-preserved"));
    auto staleRows = snapshot(ribbon, changedDock);
    exact(changedDock.collection->RemoveAt(0), "One independent native row removal");
    auto changedRows = readNative(ribbon, window.handle, deadline, "native-row-mutation");
    require(ribbon.editQuickAccess(staleRows, {RibbonQuickAccessEditKind::Add, Copy, 0, 0}) == HRESULT_FROM_WIN32(ERROR_RETRY), "Stale row edit overwrote native state");
    exactState(changedRows, readNative(ribbon, window.handle, deadline, "stale-row-preserved"));
    exact(changedRows.collection->Insert(0, changedDock.rows.front().object.Get()), "Restore only the independently removed native row");
    exactState(changedDock, readNative(ribbon, window.handle, deadline, "native-row-restored"));

    // Setup uses the unchanged public native setter with actual declared IDs.
    // The oracle below reads the real framework collection, not a host model.
    const std::array<UINT, 20> capacity{Copy, Cut, Paste, CopyPath, PasteShortcut, Rename, NewFolder, Properties, Open, Edit,
        Print, Undo, SelectAll, SelectNone, Invert, PreviewPane, DetailsPane, HiddenItems, Extensions, Checkboxes};
    exact(ribbon.setQuickAccessCommands(capacity), "Seed20 distinct declared commands through existing native setup API");
    materialize(ribbon, window.handle, deadline);
    auto twenty = readNative(ribbon, window.handle, deadline, "actual-twenty"); ids(twenty, ribbon, capacity);
    auto twentySnapshot = snapshot(ribbon, twenty);
    require(twentySnapshot.items().size() == 20, "Native20 command snapshot was truncated");
    require(ribbon.editQuickAccess(twentySnapshot, {RibbonQuickAccessEditKind::Add, FileHistory, 0, 0}) == HRESULT_FROM_WIN32(ERROR_TOO_MANY_CMDS), "Native20 capacity did not reject the21st command");
    exactState(twenty, readNative(ribbon, window.handle, deadline, "capacity-rejection-preserved"));
    edit(ribbon, twentySnapshot, RibbonQuickAccessEditKind::Move, 0, 19, 0);
    auto rotated = twenty; std::rotate(rotated.rows.begin(), rotated.rows.end() - 1, rotated.rows.end());
    exactState(rotated, readNative(ribbon, window.handle, deadline, "twenty-move-preserved"));
    auto rotatedIds = capacity; std::rotate(rotatedIds.begin(), rotatedIds.end() - 1, rotatedIds.end());
    const auto layoutName = layout == RibbonLayout::Authored ? L"authored" : L"installed";
    const auto capacitySettings = files.path / (std::wstring(layoutName) + L"-twenty.bin");
    require(files.current() && !fs::exists(capacitySettings), "Native20 save must create a new exact owned settings file");
    exact(ribbon.saveSettings(capacitySettings), "Save actual native20 command settings");
    require(fs::is_regular_file(capacitySettings) && fs::file_size(capacitySettings) > 0 && fs::file_size(capacitySettings) <= 65536,
        "Actual native20 saved settings file is absent or unbounded");
    exactState(rotated, readNative(ribbon, window.handle, deadline, "twenty-save-preserved"));
    exact(rotated.collection->RemoveAt(19), "Disturb actual native20 collection before reload"); dock(rotated, rotated.dock != UI_CONTROLDOCK_BOTTOM);
    exact(ribbon.loadSettings(capacitySettings), "Load actual native20 saved settings"); materialize(ribbon, window.handle, deadline);
    auto restoredTwenty = readNative(ribbon, window.handle, deadline, "twenty-settings-restored"); ids(restoredTwenty, ribbon, rotatedIds);
    require(restoredTwenty.dock == rotated.dock && snapshot(ribbon, restoredTwenty).items().size() == 20,
        "Native20 saved state was truncated or lost its dock");

    // This is public runtime collection import, not a context-menu gesture.
    // Import the actual retained native Undo row, which is outside the chooser.
    exact(restoredTwenty.collection->Clear(), "Clear only this current owned setup collection");
    exact(restoredTwenty.collection->Add(initial.rows[0].object.Get()), "Import actual retained Properties row");
    exact(restoredTwenty.collection->Add(initial.rows[1].object.Get()), "Import actual retained NewFolder row");
    exact(restoredTwenty.collection->Add(twenty.rows[11].object.Get()), "Import actual declared Undo row through public native collection");
    materialize(ribbon, window.handle, deadline);
    auto imported = readNative(ribbon, window.handle, deadline, "native-runtime-import");
    const std::array<UINT, 3> importedIds{Properties, NewFolder, Undo}; ids(imported, ribbon, importedIds);
    require(sameRow(imported.rows[0], initial.rows[0]) && sameRow(imported.rows[1], initial.rows[1]) &&
        sameRow(imported.rows[2], twenty.rows[11]), "Public native collection import replaced retained objects");
    for (const bool below : {false, true}) {
        dock(imported, below);
        auto saved = readNative(ribbon, window.handle, deadline, "before-native-save"); ids(saved, ribbon, importedIds);
        const auto settings = files.path / (std::wstring(layoutName) + (below ? L"-below.bin" : L"-above.bin"));
        require(files.current() && !fs::exists(settings), "Native save must create a new exact owned settings file");
        exact(ribbon.saveSettings(settings), "Save genuine owned native settings");
        require(fs::is_regular_file(settings) && fs::file_size(settings) > 0 && fs::file_size(settings) <= 65536, "Actual saved settings file is absent or unbounded");
        exactState(saved, readNative(ribbon, window.handle, deadline, "save-preserved"));
        exact(saved.collection->RemoveAt(2), "Disturb actual native Undo before reload"); dock(saved, !below);
        exact(ribbon.loadSettings(settings), "Load genuine saved native settings"); materialize(ribbon, window.handle, deadline);
        auto restored = readNative(ribbon, window.handle, deadline, "native-settings-restored"); ids(restored, ribbon, importedIds);
        require(restored.dock == static_cast<ULONG>(below ? UI_CONTROLDOCK_BOTTOM : UI_CONTROLDOCK_TOP), "Saved native dock/order was lost");
        // Serialized reload may reconstruct row COM objects. Preserve exact
        // identities starting with these real post-load objects for each edit.
        auto loadedSnapshot = snapshot(ribbon, restored);
        edit(ribbon, loadedSnapshot, RibbonQuickAccessEditKind::Add, Copy, 0, 0);
        auto postLoadAdd = readNative(ribbon, window.handle, deadline, "post-load-add");
        const std::array<UINT, 4> postLoadIds{Properties, NewFolder, Undo, Copy}; ids(postLoadAdd, ribbon, postLoadIds);
        require(postLoadAdd.rows.size() == 4, "Post-load Add lost the actual noncatalog row");
        for (size_t index = 0; index < restored.rows.size(); ++index)
            require(sameRow(restored.rows[index], postLoadAdd.rows[index]), "Post-load Add rebuilt a retained saved row");
        auto postLoadSnapshot = snapshot(ribbon, postLoadAdd);
        edit(ribbon, postLoadSnapshot, RibbonQuickAccessEditKind::Remove, 0, 3, 0);
        exactState(restored, readNative(ribbon, window.handle, deadline, "post-load-remove"));
        auto reorderSnapshot = snapshot(ribbon, restored);
        edit(ribbon, reorderSnapshot, RibbonQuickAccessEditKind::Move, 0, 2, 1);
        auto reordered = restored; std::swap(reordered.rows[1], reordered.rows[2]);
        exactState(reordered, readNative(ribbon, window.handle, deadline, "post-load-noncatalog-move"));
        auto undoReorder = snapshot(ribbon, reordered);
        edit(ribbon, undoReorder, RibbonQuickAccessEditKind::Move, 0, 1, 2);
        exactState(restored, readNative(ribbon, window.handle, deadline, "post-load-noncatalog-order-restored"));
        imported = std::move(restored);
    }
    require(executions == 0, "QAT regression invoked an actual Shell command");
    isolation(window.handle, deadline);
    std::cout << "PASS native QAT layout=" << label << " exactAddRemoveMove=1 actualCapacity20=1 savedAboveBelow=1 noncatalogUndoPreserved=1"
        << " contextMenuGesture=NOT_COVERED opaqueSeparator=NOT_COVERED" << std::endl;
}

UINT envelopeUInt(const std::vector<BYTE>& bytes, size_t offset) {
    require(offset + 4 <= bytes.size(), "Read bounded envelope field");
    return bytes[offset] | (UINT(bytes[offset + 1]) << 8) | (UINT(bytes[offset + 2]) << 16) | (UINT(bytes[offset + 3]) << 24);
}
void envelopeUInt(std::vector<BYTE>& bytes, size_t offset, UINT value) {
    require(offset + 4 <= bytes.size(), "Write bounded adversarial envelope field");
    for (unsigned part = 0; part < 4; ++part) bytes[offset + part] = static_cast<BYTE>(value >> (part * 8));
}
std::array<BYTE, 32> envelopeDigest(const std::vector<BYTE>& bytes) {
    require(bytes.size() >= 56, "Checksum fixture has complete envelope header");
    struct Algorithm { BCRYPT_ALG_HANDLE value = nullptr; ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); } } algorithm;
    require(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Open fixture SHA256 provider");
    DWORD length = 0, written = 0;
    const auto property = BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &written, 0);
    if (property < 0 || !length || written != sizeof(length)) throw std::runtime_error("Fixture hash-object length");
    std::vector<BYTE> storage(length); std::array<BYTE, 32> result{}, zero{};
    struct Hash { BCRYPT_HASH_HANDLE value = nullptr; ~Hash() { if (value) BCryptDestroyHash(value); } } hash;
    auto status = BCryptCreateHash(algorithm.value, &hash.value, storage.data(), length, nullptr, 0, 0);
    if (status >= 0) status = BCryptHashData(hash.value, const_cast<BYTE*>(bytes.data()), 24, 0);
    if (status >= 0) status = BCryptHashData(hash.value, zero.data(), static_cast<ULONG>(zero.size()), 0);
    if (status >= 0) status = BCryptHashData(hash.value, const_cast<BYTE*>(bytes.data() + 56), static_cast<ULONG>(bytes.size() - 56), 0);
    if (status >= 0) status = BCryptFinishHash(hash.value, result.data(), static_cast<ULONG>(result.size()), 0);
    require(status >= 0, "Compute independent adversarial fixture checksum"); return result;
}
void rehashEnvelope(std::vector<BYTE>& bytes) {
    const auto digest = envelopeDigest(bytes); std::copy(digest.begin(), digest.end(), bytes.begin() + 24);
}
std::vector<BYTE> readOwnedBytes(const Directory& files, const fs::path& path) {
    require(files.current() && path.parent_path() == files.path, "Read only current owned settings fixture");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Open owned fixture bytes");
    LARGE_INTEGER length{}; std::vector<BYTE> bytes;
    const bool sized = GetFileSizeEx(file, &length) != FALSE;
    if (sized && length.QuadPart > 0 && length.QuadPart <= 65700) bytes.resize(static_cast<size_t>(length.QuadPart));
    DWORD read = 0; const bool complete = !bytes.empty() && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE && read == bytes.size();
    const bool closed = CloseHandle(file) != FALSE; require(sized && complete && closed, "Read complete bounded owned fixture"); return bytes;
}
void writeOwnedBytes(const Directory& files, const fs::path& path, const std::vector<BYTE>& bytes) {
    require(files.current() && path.parent_path() == files.path && !bytes.empty() && bytes.size() <= 65700, "Write only bounded current owned fixture");
    const auto name = path.filename().wstring(); require(name == L"authored-invalid.bin" || name == L"installed-invalid.bin" ||
        name == L"authored-legacy.bin" || name == L"installed-legacy.bin", "Refuse an unowned negative-fixture filename");
    const DWORD attributes = GetFileAttributesW(path.c_str());
    require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "Negative fixture is not a replaced directory/link");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Open exact owned negative fixture"); DWORD written = 0;
    const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size();
    const bool flushed = complete && FlushFileBuffers(file) != FALSE, closed = CloseHandle(file) != FALSE;
    require(complete && flushed && closed && files.current(), "Publish complete owned adversarial fixture");
}
bool nativeMinimized(const NativeState& state) {
    Variant value; exact(state.view->GetValue(UI_PKEY_Minimized, &value.value), "Read independent actual minimized state");
    BOOL minimized = FALSE; exact(PropVariantToBoolean(value.value, &minimized), "Convert independent minimized state"); return minimized != FALSE;
}
std::vector<UINT> nativeIds(const NativeState& state) {
    std::vector<UINT> result; result.reserve(state.rows.size());
    for (const auto& row : state.rows) { require(row.commandRead == S_OK && row.commandConvert == S_OK && row.command, "Every loaded actual ID is public and readable"); result.push_back(row.command); }
    return result;
}
class MutableQatRow final : public IUISimplePropertySet {
public:
    UINT command = 0, type = UI_COMMANDTYPE_ACTION;
    std::wstring label = L"Exact Native Metadata";
    bool enabled = true;
    std::function<void()> once;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IUISimplePropertySet)) return E_NOINTERFACE;
        *output = static_cast<IUISimplePropertySet*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* output) override {
        if (!output) return E_POINTER; PropVariantInit(output);
        if (once) { auto callback = std::move(once); once = {}; callback(); }
        if (IsEqualPropertyKey(key, UI_PKEY_CommandId)) return InitPropVariantFromUInt32(command, output);
        if (IsEqualPropertyKey(key, UI_PKEY_CommandType)) return InitPropVariantFromUInt32(type, output);
        if (IsEqualPropertyKey(key, UI_PKEY_Label)) return InitPropVariantFromString(label.c_str(), output);
        if (IsEqualPropertyKey(key, UI_PKEY_Enabled)) return InitPropVariantFromBoolean(enabled, output);
        return E_NOTIMPL;
    }
private:
    ULONG references_ = 1;
};
class QatChangeSink final : public IUICollectionChangedEvent {
public:
    ComPtr<IUICollection> collection;
    std::vector<ComPtr<IUnknown>> seeds;
    std::function<HRESULT()> reenter;
    bool fullLoaded = false, triggered = false;
    HRESULT callback = E_PENDING;
    unsigned movedWholeRows = 0;
    ComPtr<IUnknown> removed;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IUICollectionChangedEvent)) return E_NOINTERFACE;
        *output = static_cast<IUICollectionChangedEvent*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnChanged(UI_COLLECTIONCHANGE action, UINT32, IUnknown* oldItem, UINT32, IUnknown* newItem) override {
        try {
            UINT count = 0; const auto counted = collection->GetCount(&count); if (counted != S_OK) return counted;
            ComPtr<IUnknown> oldIdentity, newIdentity;
            if (oldItem) { const auto read = oldItem->QueryInterface(IID_PPV_ARGS(&oldIdentity)); if (read != S_OK) return read; }
            if (newItem) { const auto read = newItem->QueryInterface(IID_PPV_ARGS(&newIdentity)); if (read != S_OK) return read; }
            const auto seeded = [&](IUnknown* identity) { return std::any_of(seeds.begin(), seeds.end(), [&](const auto& seed) { return seed.Get() == identity; }); };
            if (count == 20 && (action == UI_COLLECTIONCHANGE_RESET || (newIdentity && !seeded(newIdentity.Get())))) fullLoaded = true;
            if (action == UI_COLLECTIONCHANGE_INSERT && removed && newIdentity.Get() == removed.Get()) { ++movedWholeRows; removed.Reset(); }
            if (action == UI_COLLECTIONCHANGE_REMOVE && fullLoaded && count == 19 && oldIdentity && !seeded(oldIdentity.Get())) {
                removed = oldIdentity;
                if (!triggered && reenter) { triggered = true; callback = reenter(); }
            }
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
private:
    ULONG references_ = 1;
};
struct QatAdvice {
    ComPtr<IConnectionPoint> point;
    DWORD cookie = 0;
    QatAdvice(IUICollection* collection, IUICollectionChangedEvent* sink) {
        ComPtr<IConnectionPointContainer> container; exact(collection->QueryInterface(IID_PPV_ARGS(&container)), "Public native collection event source");
        exact(container->FindConnectionPoint(__uuidof(IUICollectionChangedEvent), &point), "Find actual native collection change events");
        exact(point->Advise(sink, &cookie), "Observe exact actual native collection operations"); require(cookie != 0, "Collection observer cookie is missing");
    }
    ~QatAdvice() { if (cookie && point->Unadvise(cookie) != S_OK) { std::cerr << "FAIL QAT event observer cleanup" << std::endl; std::_Exit(9); } }
};
void runSettingsEnvelopeLayout(explorer::RibbonLayout layout, ULONGLONG deadline) {
    using namespace explorer;
    Directory files; const std::wstring name = layout == RibbonLayout::Authored ? L"authored" : L"installed";
    const auto settings = files.path / (name + L"-envelope.bin"), invalid = files.path / (name + L"-invalid.bin"), legacy = files.path / (name + L"-legacy.bin");
    Window window{CreateWindowExW(0, L"WindowsExplorerNativeQATRegression", L"Owned QAT settings envelope", WS_OVERLAPPEDWINDOW,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    require(window.handle != nullptr, "Create private settings-envelope owner");
    NativeRibbon ribbon; unsigned executions = 0;
    const auto callbacks = [&] { RibbonCallbacks result; result.execute = [&](UINT) { ++executions; return E_ACCESSDENIED; };
        result.query = [](UINT command) { RibbonCommandState state; state.enabled = command != Copy; return state; }; return result; };
    exact(ribbon.initialize(window.handle, GetModuleHandleW(nullptr), callbacks(), layout), "Initialize actual envelope-layout framework");
    require(ribbon.layout() == layout && (layout != RibbonLayout::InstalledWindows10 || ribbon.installedLayoutStatus() == S_OK), "Envelope requested layout must be genuine");
    materialize(ribbon, window.handle, deadline);
    const std::array<UINT, 20> order{Checkboxes, Copy, Cut, Paste, CopyPath, PasteShortcut, Rename, NewFolder, Properties, Open,
        Edit, Print, Undo, SelectAll, SelectNone, Invert, PreviewPane, DetailsPane, HiddenItems, Extensions};
    exact(ribbon.setQuickAccessCommands(order), "Seed rotated actual20 envelope commands");
    exact(ribbon.setQuickAccessBelow(true), "Seed actual saved bottom dock"); exact(ribbon.setMinimized(true), "Seed actual saved minimized state");
    auto saved = readNative(ribbon, window.handle, deadline, "envelope-save"); ids(saved, ribbon, order); require(nativeMinimized(saved), "Saved minimized setup");
    exact(ribbon.saveSettings(settings), "Atomic ordered native settings save"); exactState(saved, readNative(ribbon, window.handle, deadline, "envelope-save-preserved"));
    const auto bytes = readOwnedBytes(files, settings); constexpr std::array<BYTE, 8> magic{'W', 'E', 'Q', 'A', 'T', '\r', '\n', 0x1a};
    require(bytes.size() >= 136 && std::equal(magic.begin(), magic.end(), bytes.begin()) && envelopeUInt(bytes, 8) == 1 &&
        envelopeUInt(bytes, 12) == static_cast<UINT>(layout) && envelopeUInt(bytes, 16) == 20, "Saved envelope schema/layout/capacity");
    const auto length = envelopeUInt(bytes, 20); require(length && length <= 65536 && bytes.size() == 136 + length, "Exact opaque native payload bounds");
    const auto digest = envelopeDigest(bytes); require(std::equal(digest.begin(), digest.end(), bytes.begin() + 24), "Saved whole-envelope checksum");
    for (UINT index = 0; index < 20; ++index) require(envelopeUInt(bytes, 56 + index * 4) == ribbon.nativeCommandId(order[index]), "Manifest retains actual native ID order");
    const std::array<UINT, 2> disturbance{Properties, NewFolder};
    exact(ribbon.setQuickAccessCommands(disturbance), "Disturb envelope command count"); exact(ribbon.setQuickAccessBelow(false), "Disturb native dock");
    exact(ribbon.setMinimized(false), "Disturb native minimized state");
    auto disturbed = readNative(ribbon, window.handle, deadline, "envelope-disturbed");
    ComPtr<QatChangeSink> observer; observer.Attach(new QatChangeSink); observer->collection = disturbed.collection;
    for (const auto& row : disturbed.rows) observer->seeds.push_back(row.identity);
    { QatAdvice advice(disturbed.collection.Get(), observer.Get()); exact(ribbon.loadSettings(settings), "Load envelope and move only actual loaded whole rows"); }
    auto restored = readNative(ribbon, window.handle, deadline, "envelope-restored"); ids(restored, ribbon, order);
    require(restored.dock == UI_CONTROLDOCK_BOTTOM && nativeMinimized(restored), "Native Load supplies saved dock and minimized state");
    require(observer->movedWholeRows > 0, "Observe actual loaded whole-row remove/insert ordering correction");
    const auto rejected = [&](std::vector<BYTE> adversarial, const char* phase) {
        writeOwnedBytes(files, invalid, adversarial); const auto before = readNative(ribbon, window.handle, deadline, phase); const auto minimized = nativeMinimized(before);
        require(ribbon.loadSettings(invalid) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid envelope must fail strict structural/checksum validation");
        const auto after = readNative(ribbon, window.handle, deadline, "invalid-before-native-preserved"); exactState(before, after);
        require(nativeMinimized(after) == minimized, "Invalid envelope changed actual native minimized state");
    };
    auto bad = bytes; bad[24] ^= 1; rejected(bad, "checksum-corruption");
    bad = bytes; envelopeUInt(bad, 8, 2); rehashEnvelope(bad); rejected(bad, "version-mismatch");
    bad = bytes; envelopeUInt(bad, 12, layout == RibbonLayout::Authored ? 1 : 0); rehashEnvelope(bad); rejected(bad, "layout-mismatch");
    bad = bytes; envelopeUInt(bad, 16, 21); rejected(bad, "capacity-overflow");
    bad = bytes; envelopeUInt(bad, 20, 0xffffffff); rejected(bad, "length-overflow");
    bad = bytes; envelopeUInt(bad, 56, 0); rehashEnvelope(bad); rejected(bad, "zero-native-id");
    bad = bytes; envelopeUInt(bad, 60, envelopeUInt(bad, 56)); rehashEnvelope(bad); rejected(bad, "duplicate-native-id");
    bad = bytes; bad.pop_back(); rejected(bad, "truncated-envelope");
    bad = bytes; bad.push_back(0); rejected(bad, "trailing-envelope-byte");
    bad = bytes; bad.back() ^= 1; rejected(bad, "opaque-payload-checksum-corruption");
    // Valid checksums with incompatible manifests reach real native Load, then
    // require exact original whole-row/state/dock rollback before reporting.
    exact(ribbon.setQuickAccessCommands(disturbance), "Seed original rows for mismatch rollback");
    exact(ribbon.setQuickAccessBelow(false), "Seed original rollback dock"); exact(ribbon.setMinimized(false), "Seed original rollback state");
    auto rollbackBefore = readNative(ribbon, window.handle, deadline, "before-set-mismatch");
    bad = bytes; envelopeUInt(bad, 56, ribbon.nativeCommandId(FileHistory)); rehashEnvelope(bad);
    writeOwnedBytes(files, invalid, bad); require(ribbon.loadSettings(invalid) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Loaded exact native ID set mismatch must reject");
    exactState(rollbackBefore, readNative(ribbon, window.handle, deadline, "set-mismatch-original-rows-restored")); require(!nativeMinimized(rollbackBefore), "Set mismatch original minimized state restored");
    bad = bytes; bad.erase(bad.begin() + 56, bad.begin() + 60); envelopeUInt(bad, 16, 19); rehashEnvelope(bad);
    writeOwnedBytes(files, invalid, bad); require(ribbon.loadSettings(invalid) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Loaded exact count mismatch must reject");
    exactState(rollbackBefore, readNative(ribbon, window.handle, deadline, "count-mismatch-original-rows-restored"));
    require(!nativeMinimized(rollbackBefore), "Count mismatch original minimized state restored");
    // A failed atomic replacement preserves the original complete envelope.
    const DWORD attributes = GetFileAttributesW(settings.c_str()); require(attributes != INVALID_FILE_ATTRIBUTES, "Read exact owned save attributes");
    require(SetFileAttributesW(settings.c_str(), attributes | FILE_ATTRIBUTE_READONLY) != FALSE, "Protect owned destination for atomic failure control");
    const auto failedSave = ribbon.saveSettings(settings);
    require(SetFileAttributesW(settings.c_str(), attributes) != FALSE, "Restore only original owned destination attributes");
    require(FAILED(failedSave) && readOwnedBytes(files, settings) == bytes, "Atomic save failure changed original complete file");
    exactState(rollbackBefore, readNative(ribbon, window.handle, deadline, "atomic-failure-source-preserved"));
    // Legacy raw settings remain delegated to native. Compare actual native
    // direct-load order rather than inventing a historical custom order.
    const std::vector<BYTE> payload(bytes.begin() + 136, bytes.end()); writeOwnedBytes(files, legacy, payload);
    ComPtr<IUIRibbon> nativeView; exact(ribbon.nativeFramework()->GetView(0, IID_PPV_ARGS(&nativeView)), "Retain actual legacy native view");
    ComPtr<IStream> raw; raw.Attach(SHCreateMemStream(payload.data(), static_cast<UINT>(payload.size()))); require(raw != nullptr, "Create unchanged opaque legacy stream");
    exact(nativeView->LoadSettingsFromStream(raw.Get()), "Actual direct opaque legacy native Load");
    auto directLegacy = readNative(ribbon, window.handle, deadline, "direct-legacy-reference"); const auto rawOrder = nativeIds(directLegacy);
    const auto rawMinimized = nativeMinimized(directLegacy);
    exact(ribbon.setQuickAccessCommands(disturbance), "Disturb legacy fixture"); exact(ribbon.setQuickAccessBelow(false), "Disturb legacy dock"); exact(ribbon.setMinimized(false), "Disturb legacy state");
    exact(ribbon.loadSettings(legacy), "Backward compatible opaque legacy native stream load");
    auto legacyRestored = readNative(ribbon, window.handle, deadline, "legacy-restored");
    require(nativeIds(legacyRestored) == rawOrder && legacyRestored.dock == directLegacy.dock && nativeMinimized(legacyRestored) == rawMinimized, "Legacy follows actual native order/state/dock");
    // In-place metadata edits do not emit collection events or host revisions.
    // The exact public-property binding fence must still reject a stale edit.
    exact(ribbon.setQuickAccessCommands(disturbance), "Seed mutable-property binding fixture");
    auto metadataBefore = readNative(ribbon, window.handle, deadline, "metadata-seed");
    ComPtr<MutableQatRow> mutableRow; mutableRow.Attach(new MutableQatRow); mutableRow->command = ribbon.nativeCommandId(NewFolder);
    exact(metadataBefore.collection->RemoveAt(1), "Replace only one owned metadata test row"); exact(metadataBefore.collection->Insert(1, mutableRow.Get()), "Insert exact public mutable property row");
    auto metadata = readNative(ribbon, window.handle, deadline, "metadata-imported"); auto metadataSnapshot = snapshot(ribbon, metadata);
    mutableRow->label = L"exact native metadata";
    require(ribbon.editQuickAccess(metadataSnapshot, {RibbonQuickAccessEditKind::Add, Copy, 0, 0}) == HRESULT_FROM_WIN32(ERROR_RETRY), "Case-only in-place native label change must stale the snapshot");
    auto changedMetadata = readNative(ribbon, window.handle, deadline, "changed-native-label");
    require(changedMetadata.rows.size() == 2 && changedMetadata.rows[1].identity.Get() == metadata.rows[1].identity.Get() && changedMetadata.rows[1].label == mutableRow->label, "Stale metadata rejection rebuilt the actual row");
    auto flagSnapshot = snapshot(ribbon, changedMetadata); mutableRow->enabled = false;
    require(ribbon.editQuickAccess(flagSnapshot, {RibbonQuickAccessEditKind::Remove, 0, 1, 0}) == HRESULT_FROM_WIN32(ERROR_RETRY), "In-place public enabled flag change must stale a removal");
    require(changedMetadata.rows[1].identity.Get() == readNative(ribbon, window.handle, deadline, "changed-row-flag-preserved").rows[1].identity.Get(), "Stale flag removal discarded the actual object");
    mutableRow->enabled = true;
    auto typeState = readNative(ribbon, window.handle, deadline, "before-row-type-mutation"); auto typeSnapshot = snapshot(ribbon, typeState);
    mutableRow->type = UI_COMMANDTYPE_BOOLEAN;
    require(ribbon.editQuickAccess(typeSnapshot, {RibbonQuickAccessEditKind::Move, 0, 1, 0}) == HRESULT_FROM_WIN32(ERROR_RETRY), "In-place public native row-type change must stale a move");
    require(readNative(ribbon, window.handle, deadline, "changed-row-type-preserved").rows[1].type == UI_COMMANDTYPE_BOOLEAN, "Stale type move rebuilt the native property row");
    mutableRow->type = UI_COMMANDTYPE_ACTION;
    exact(ribbon.setQuickAccessBelow(false), "Seed source-fence dock"); auto sourceBefore = readNative(ribbon, window.handle, deadline, "source-before-save-reentry");
    mutableRow->once = [&] { exact(ribbon.setQuickAccessBelow(true), "Newer writer during real row-property save observation"); };
    require(ribbon.saveSettings(settings) == HRESULT_FROM_WIN32(ERROR_RETRY), "Save must reject a newer reentrant QAT writer");
    require(readOwnedBytes(files, settings) == bytes, "Stale save published a mismatched native/order envelope");
    auto sourceAfter = readNative(ribbon, window.handle, deadline, "source-save-newer-edit-preserved");
    require(sourceAfter.dock == UI_CONTROLDOCK_BOTTOM && sourceAfter.rows.size() == sourceBefore.rows.size(), "Save source fence overwrote a newer native dock edit");
    // Trigger a host writer at the first public removal of a newly loaded row,
    // after native has populated all20. The wrapper must stop and refuse rollback.
    exact(ribbon.setQuickAccessCommands(disturbance), "Seed load-reentry original rows"); exact(ribbon.setMinimized(false), "Seed load-reentry original state");
    auto reentryBefore = readNative(ribbon, window.handle, deadline, "load-reentry-seed");
    ComPtr<QatChangeSink> reentry; reentry.Attach(new QatChangeSink); reentry->collection = reentryBefore.collection;
    for (const auto& row : reentryBefore.rows) reentry->seeds.push_back(row.identity);
    reentry->reenter = [&] { return ribbon.setMinimized(false); };
    { QatAdvice advice(reentryBefore.collection.Get(), reentry.Get()); require(ribbon.loadSettings(settings) == HRESULT_FROM_WIN32(ERROR_RETRY), "Reentrant genuine state writer must make load return truthful retry"); }
    require(reentry->triggered && reentry->callback == S_OK, "Actual loaded-row reorder did not execute reentry control");
    auto newer = readNative(ribbon, window.handle, deadline, "newer-load-edit-preserved");
    require(!nativeMinimized(newer) && newer.rows.size() == 19, "Failed load rolled back across a newer edit or continued its old move");
    // Reset inside the same real native collection notification retires the
    // binding. Retained native interfaces must unwind safely; old snapshots
    // cannot target a subsequently initialized framework on the same HWND.
    exact(ribbon.setQuickAccessCommands(disturbance), "Seed reset-reentry original rows");
    auto resetBefore = readNative(ribbon, window.handle, deadline, "load-reset-seed"); auto staleReset = snapshot(ribbon, resetBefore);
    ComPtr<QatChangeSink> resetSink; resetSink.Attach(new QatChangeSink); resetSink->collection = resetBefore.collection;
    for (const auto& row : resetBefore.rows) resetSink->seeds.push_back(row.identity);
    resetSink->reenter = [&] { ribbon.reset(); return S_OK; };
    { QatAdvice advice(resetBefore.collection.Get(), resetSink.Get()); require(ribbon.loadSettings(settings) == HRESULT_FROM_WIN32(ERROR_RETRY), "Native callback reset must retire and safely abort old load"); }
    require(resetSink->triggered && !ribbon.valid(), "Reset control did not retire actual native source");
    exact(ribbon.initialize(window.handle, GetModuleHandleW(nullptr), callbacks(), layout), "Create fresh framework on same owned HWND after reset");
    auto fresh = readNative(ribbon, window.handle, deadline, "fresh-framework-after-reset");
    require(ribbon.editQuickAccess(staleReset, {RibbonQuickAccessEditKind::Add, Copy, 0, 0}) == HRESULT_FROM_WIN32(ERROR_RETRY), "Retired generation snapshot targeted fresh framework");
    exactState(fresh, readNative(ribbon, window.handle, deadline, "stale-generation-preserved"));
    require(executions == 0, "Envelope regression executed a Shell command"); isolation(window.handle, deadline);
    std::cout << "PASS QAT settings envelope layout=" << static_cast<UINT>(layout) << " ordered20=1 opaqueLegacy=1 corruptBeforeMutation=10"
        << " exactSetCountRollback=1 atomicFailure=1 metadataFence=1 sourceReentry=1 newerLoadEdit=1 retiredGeneration=1" << std::endl;
}
int runSettingsEnvelopeControls(ULONGLONG deadline) {
    runSettingsEnvelopeLayout(explorer::RibbonLayout::Authored, deadline);
    runSettingsEnvelopeLayout(explorer::RibbonLayout::InstalledWindows10, deadline);
    std::cout << "PASS QAT settings envelope actual layouts=2 assertions=" << assertions << std::endl; return 0;
}
} // namespace

int main(int argc, char** argv) {
    const bool settingsControl = argc == 2 && std::string_view(argv[1]) == "--settings-envelope-control";
    if (argc != 1 && !settingsControl) { std::cerr << "Usage: native_quick_access_tests [--settings-envelope-control]" << std::endl; return 2; }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    explorer::PrivateDesktop desktop;
    const auto guard = desktop.initialize();
    if (guard != S_OK) { std::cerr << "FAIL private desktop HRESULT=" << static_cast<ULONG>(guard) << std::endl; return 2; }
    const auto apartment = OleInitialize(nullptr);
    if (FAILED(apartment)) { std::cerr << "FAIL native STA HRESULT=" << static_cast<ULONG>(apartment) << std::endl; return 3; }
    int result = 0;
    try {
        const ULONGLONG deadline = GetTickCount64() + 30000;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "Initialize native Common Controls v6");
        WNDCLASSW host{}; host.lpfnWndProc = DefWindowProcW; host.hInstance = GetModuleHandleW(nullptr);
        host.lpszClassName = L"WindowsExplorerNativeQATRegression";
        require(RegisterClassW(&host) != 0, "Register exact private native QAT owner class");
        if (settingsControl) result = runSettingsEnvelopeControls(deadline);
        else {
            Directory files;
            runLayout(explorer::RibbonLayout::Authored, files, deadline);
            runLayout(explorer::RibbonLayout::InstalledWindows10, files, deadline);
        }
        require(UnregisterClassW(host.lpszClassName, host.hInstance) != FALSE, "Release owned QAT host class");
        if (!settingsControl) std::cout << "PASS native QAT real framework regression assertions=" << assertions << std::endl;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << std::endl; result = 1; }
    OleUninitialize();
    return result;
}
