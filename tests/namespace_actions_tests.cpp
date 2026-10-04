#include "explorer/namespace_actions.hpp"
#include "explorer/headless_visual.hpp"
#include "explorer/worker_sta.hpp"

#include <shlobj.h>
#include <wrl/implements.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
using explorer::NamespaceAction;
using explorer::NamespaceFacts;
using explorer::NamespaceInvocationPlan;
using explorer::NamespaceInvocationRoute;
namespace fs = std::filesystem;
unsigned assertions = 0;

void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

void succeeded(HRESULT value, const char* message) {
    ++assertions;
    if (FAILED(value)) {
        std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(value) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}

void onPrivateNamespaceDesktop(const std::function<void()>& body) {
    std::exception_ptr failure;
    std::atomic<bool> complete{false};
    std::thread worker([&] {
        explorer::PrivateDesktop desktop;bool initialized=false;
        try {
            succeeded(desktop.initialize(),"Attach private namespace desktop before native initialization");
            succeeded(desktop.verifyIsolation(),"Verify private namespace desktop isolation");
            succeeded(OleInitialize(nullptr),"Initialize private namespace STA");initialized=true;
            body();
            bool visible=true;
            succeeded(desktop.visibleWindowsOnInputDesktop(visible),"Inspect namespace fixture input-desktop visibility");
            require(!visible,"Namespace fixture displayed an input-desktop window");
            succeeded(desktop.verifyIsolation(),"Verify input desktop remains unchanged after namespace fixture");
        } catch(...) { failure=std::current_exception(); }
        if(initialized) {
            try{succeeded(explorer::drainStaWorkers(10000),"Drain namespace fixture workers before apartment shutdown");}
            catch(...){if(!failure)failure=std::current_exception();}
            OleUninitialize();
        }
        complete.store(true);
    });
    while(!complete.load()) {
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        Sleep(1);
    }
    worker.join();if(failure)std::rethrow_exception(failure);
}

explorer::ContextMenuEntry entry(UINT id, const wchar_t* verb, UINT state = MFS_ENABLED) {
    explorer::ContextMenuEntry result;
    result.id = id;
    result.canonicalVerb = verb;
    result.label = L"\u65E5\u672C\u8A9E \u03BB translated item";
    result.state = state;
    return result;
}

struct Fixture {
    fs::path root;
    fs::path image;
    fs::path program;
    fs::path disc;
    fs::path text;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "Generate namespace fixture identity");
        wchar_t formatted[40]{};
        require(StringFromGUID2(guid, formatted, 40) != 0, "Format namespace fixture identity");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-Namespace-") + formatted);
        require(fs::create_directory(root), "Create owned namespace fixture");
        image = root / L"\u65E5\u672C\u8A9E \u03BB.bmp";
        program = root / L"headless fixture.exe";
        disc = root / L"test image.iso";
        text = root / L"document.txt";
        // Real 2x2 24-bit BMP, with two DWORD-aligned rows. No decoder/plugin UI.
        const std::array<unsigned char, 70> bitmap{
            'B','M',70,0,0,0, 0,0,0,0, 54,0,0,0,
            40,0,0,0, 2,0,0,0, 2,0,0,0, 1,0,24,0,
            0,0,0,0, 16,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,0,0, 0,0,0,0,
            0,0,255, 0,255,0, 0,0, 255,0,0, 255,255,255, 0,0
        };
        std::ofstream output(image, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bitmap.data()), bitmap.size());
        require(output.good(), "Write valid owned bitmap fixture");
        output.close();
        std::array<wchar_t, 32768> executable{};
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length && length < executable.size(), "Locate current console test executable");
        require(CopyFileW(executable.data(), program.c_str(), TRUE), "Copy owned executable without launching it");
        std::ofstream(disc, std::ios::binary) << "owned ISO-shaped fixture: never mounted or burned";
        std::ofstream(text, std::ios::binary) << "native namespace headless fixture stays intact";
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};

ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "Create owned namespace Shell item");
    return result;
}

ComPtr<IShellItemArray> array(IShellItem* value) {
    ComPtr<IShellItemArray> result;
    succeeded(SHCreateShellItemArrayFromShellItem(value, IID_PPV_ARGS(&result)), "Create namespace selection array");
    return result;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Read owned fixture for side-effect checks");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

NamespaceFacts selectedFile(const wchar_t* path) {
    NamespaceFacts facts;
    facts.selectionCount = 1;
    facts.filesystem = facts.physicalFiles = true;
    facts.singlePath = path;
    return facts;
}

void semanticPlanningAndCanonicalVerbs() {
    NamespaceFacts facts = selectedFile(L"C:\\owned\\picture.bmp");
    facts.images = true;
    auto native = entry(42, L"rotate90");
    auto stored = entry(501, L"Windows.rotate90");
    NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {native}, {stored}, &plan), "Plan exact image rotation verb");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::SelectionMenu && plan.commandId == 42 &&
            plan.canonicalVerb == L"rotate90" && plan.target == facts.singlePath, "Canonical rotation target or ordinal changed");
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {}, {stored}, &plan), "Plan native CommandStore fallback");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::CommandStoreMenu && plan.commandId == 501,
            "CommandStore fallback did not preserve exact native ordinal");
    native.canonicalVerb.clear();
    native.label = L"Rotate right";
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {native}, {}, &plan)),
            "Translated menu labels must never be treated as canonical verbs");
    facts.images = false;
    const auto previous = plan.canonicalVerb;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RotateRight, facts, {}, {stored}, &plan)) && plan.canonicalVerb == previous,
            "File type guard or failure output preservation is missing");
    require(explorer::planNamespaceAction(NamespaceAction::Count, facts, {}, {}, &plan) == E_INVALIDARG,
            "Unknown namespace action accepted");
    require(explorer::planNamespaceAction(NamespaceAction::Email, facts, {}, {}, nullptr) == E_POINTER,
            "Null namespace plan must return E_POINTER");
    facts = selectedFile(L"C:\\owned\\disc.vhdx");
    facts.discImages = true;
    succeeded(explorer::planNamespaceAction(NamespaceAction::MountDiscImage, facts, {entry(14,L"mount")}, {}, &plan), "Plan supported disk image mount");
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::BurnDiscImage, facts, {entry(15,L"burn")}, {}, &plan)),
            "Virtual hard disk must not be routed to ISO optical burning");
    facts.selectionCount = 2;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::MountDiscImage, facts, {entry(14,L"mount")}, {}, &plan)),
            "Single-target disk image action accepted a multi-selection");
    facts=selectedFile(L"C:\\owned\\shared.txt");facts.selectionCount=2;facts.singlePath.clear();
    for(const auto action:{NamespaceAction::ShareSpecificPeople,NamespaceAction::RemoveAccess}) {
        const auto command=std::wstring(explorer::namespaceActionCommand(action));
        succeeded(explorer::planNamespaceAction(action,facts,{},{entry(77,command.c_str())},&plan),
                  "Retain native multiple-item access command applicability");
        require(plan.enabled&&plan.commandId==77&&plan.target.empty(),"Multiple sharing targets were collapsed to a guessed single path");
        facts.filesystem=false;
        require(!explorer::namespaceActionApplicable(action,facts),"Virtual/nonfilesystem targets bypassed access semantics");
        facts.filesystem=true;
    }
}

void disabledMenusAndAmbiguousHandlers() {
    NamespaceFacts facts = selectedFile(L"C:\\owned\\image.bmp");
    facts.images = true;
    NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270",MFS_DISABLED)}, {}, &plan),
              "Describe disabled native rotation");
    require(!plan.enabled && plan.status == E_ACCESSDENIED, "Disabled handler was accepted");
    auto parent = entry(5,L"container",MFS_DISABLED);
    parent.submenu = true;
    parent.children = {entry(12,L"rotate270")};
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {parent}, {}, &plan), "Describe disabled ancestor");
    require(!plan.enabled, "Enabled child bypassed a disabled parent");
    require(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270"), entry(12,L"rotate270")}, {}, &plan) == E_UNEXPECTED,
            "Ambiguous same-level canonical handler must fail");
    require(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270"), entry(11,L"different")}, {}, &plan) == E_UNEXPECTED,
            "Duplicate native ordinal must fail even when another entry has a different verb");
    auto malformed = entry(0xffff,L"rotate270");
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {malformed}, {}, &plan)), "Foreign menu ID accepted");
    parent.state = MFS_ENABLED;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RotateLeft, facts, {entry(11,L"rotate270"),parent}, {}, &plan),
              "Prefer top-level verb to nested duplicate");
    require(plan.commandId == 11, "Nested duplicate took precedence over exact top-level command");

    facts = {};
    facts.filesystem = facts.physicalFolders = true;
    facts.singlePath = L"C:\\owned\\folder";
    auto library = entry(101,L"Windows.includeinlibrary");
    library.submenu = true;
    library.children = {entry(102,L"",MFS_DISABLED)};
    succeeded(explorer::planNamespaceAction(NamespaceAction::IncludeInLibrary, facts, {}, {library}, &plan), "Describe empty native library cascade");
    require(plan.submenu && !plan.enabled, "Library cascade without enabled choices was enabled");
    library.children[0].state = MFS_ENABLED;
    succeeded(explorer::planNamespaceAction(NamespaceAction::IncludeInLibrary, facts, {}, {library}, &plan), "Describe populated native library cascade");
    require(plan.enabled && plan.submenu && plan.commandId == 101, "Library cascade was flattened into a guessed child invocation");
}

void destructiveAndOptionalActionPlanning() {
    NamespaceFacts facts;
    facts.filesystem = facts.physicalFolders = facts.driveRoot = true;
    facts.driveType = DRIVE_FIXED;
    facts.singlePath = L"C:\\";
    NamespaceInvocationPlan plan;
    succeeded(explorer::planNamespaceAction(NamespaceAction::FormatDrive, facts, {entry(32,L"format")}, {}, &plan), "Plan native format without invocation");
    require(plan.enabled && plan.nativeConfirmation && plan.target == L"C:\\", "Formatting lost native confirmation/target");
    facts.driveType = DRIVE_REMOTE;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::FormatDrive, facts, {entry(32,L"format")}, {}, &plan)), "Network drive offered local formatting");
    facts.driveType = DRIVE_CDROM;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::OptimizeDrive, facts, {}, {entry(60,L"Windows.Defragment")}, &plan)),
            "Optical device offered filesystem optimization");
    succeeded(explorer::planNamespaceAction(NamespaceAction::EjectDrive, facts, {entry(33,L"eject")}, {}, &plan), "Plan optical eject without touching a device");
    facts.driveType = DRIVE_FIXED;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::BitLocker, facts, {}, {}, &plan)), "Missing BitLocker provider was invented");
    succeeded(explorer::planNamespaceAction(NamespaceAction::BitLocker, facts, {entry(50,L"manage-bde")}, {}, &plan), "Plan installed native BitLocker verb");
    require(plan.commandId == 50, "BitLocker command was guessed instead of retained");

    facts = {};
    facts.recycleBin = true;
    facts.recycleStatus = S_OK;
    succeeded(explorer::planNamespaceAction(NamespaceAction::EmptyRecycleBin, facts, {}, {}, &plan), "Describe empty isolated Recycle Bin facts");
    require(!plan.enabled && plan.status == HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS), "Empty Bin enabled a destructive command");
    facts.recycleItems = 17;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RestoreAll, facts, {}, {}, &plan), "Plan all-item native restore on simulated Bin facts");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::RestoreAllItems && plan.nativeConfirmation,
            "Restore-all route lost native decision handling");
    facts.recycleStatus = E_ACCESSDENIED;
    require(explorer::planNamespaceAction(NamespaceAction::EmptyRecycleBin, facts, {}, {}, &plan) == E_ACCESSDENIED,
            "Recycle Bin query HRESULT was discarded");
    facts.recycleStatus = S_OK;
    facts.selectionCount = 2;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RestoreSelected, facts, {entry(18,L"undelete")}, {}, &plan),
              "Plan selected native restore on simulated Bin facts");
    require(plan.commandId == 18, "Selected restoration guessed a physical recycle path");
    facts.recycleBin = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RestoreSelected, facts, {entry(18,L"undelete")}, {}, &plan)),
            "Physical folder offered namespace-only restoration");
}

void targetTypesAndFeatureMatrix() {
    NamespaceFacts files = selectedFile(L"C:\\owned\\file.txt");
    NamespaceInvocationPlan plan;
    for (const auto action : {NamespaceAction::Email,NamespaceAction::Fax,NamespaceAction::BurnToDisc}) {
        const auto command = std::wstring(explorer::namespaceActionCommand(action));
        succeeded(explorer::planNamespaceAction(action, files, {}, {entry(80,command.c_str())}, &plan), "Plan registered native sharing route");
        require(plan.enabled && plan.canonicalVerb == command, "Sharing route lost exact native command");
    }
    files.mailRecipientAvailable = true;
    files.mailRecipientPath = L"C:\\owned\\SendTo\\recipient.MAPIMail";
    succeeded(explorer::planNamespaceAction(NamespaceAction::Email, files, {}, {}, &plan), "Plan exact registered native SendTo mail recipient");
    require(plan.enabled && plan.route == NamespaceInvocationRoute::SendToMailRecipient &&
            plan.target == files.mailRecipientPath, "Mail fallback lost its installed target or native copy route");
    files.media = true;
    for (const auto action : {NamespaceAction::Play,NamespaceAction::PlayAll,NamespaceAction::AddToPlaylist,NamespaceAction::CastToDevice}) {
        const auto command = std::wstring(explorer::namespaceActionCommand(action));
        succeeded(explorer::planNamespaceAction(action, files, {}, {entry(81,command.c_str())}, &plan), "Plan native media route");
    }
    files.media = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::Play, files, {}, {entry(81,L"Windows.play")}, &plan)),
            "Non-media file offered media playback");
    files.images = true;
    files.castHandlerAvailable = files.castHandlerEnabled = files.castHandlerSubmenu = true;
    files.castCommandId = 9;
    succeeded(explorer::planNamespaceAction(NamespaceAction::CastToDevice, files, {}, {}, &plan),
              "Plan image casting through an isolated registered native handler");
    require(plan.route == NamespaceInvocationRoute::RegisteredHandlerMenu && plan.submenu && plan.commandId == 9,
            "Image casting guessed a translated composed-menu item");
    files.images = false;
    files.applications = true;
    succeeded(explorer::planNamespaceAction(NamespaceAction::RunAsAdministrator, files, {entry(88,L"runas")}, {}, &plan), "Plan native elevation without launching anything");
    files.applications = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::RunAsAdministrator, files, {entry(88,L"runas")}, {}, &plan)),
            "Document offered application elevation");
    NamespaceFacts network;
    network.filesystem = network.physicalFolders = network.uncPaths = true;
    network.singlePath = L"\\\\example\\share\\folder";
    succeeded(explorer::planNamespaceAction(NamespaceAction::MapAsDrive, network, {}, {entry(90,L"Windows.connectNetworkDrive")}, &plan),
              "Plan target-specific network mapping without a network request");
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::AlwaysAvailableOffline, network, {}, {}, &plan)),
            "Inactive Offline Files service was offered as an available feature");
    network.offlineActive = true;
    network.offlineStatus = S_OK;
    succeeded(explorer::planNamespaceAction(NamespaceAction::AlwaysAvailableOffline, network, {}, {}, &plan),
              "Plan public asynchronous Offline Files pinning without a network/cache mutation");
    require(plan.route == NamespaceInvocationRoute::OfflineFilesPin && plan.canonicalVerb == L"IOfflineFilesCache.Pin" && !plan.checked,
            "Offline availability lost its exact native API or pin state");
    network.offlinePinnedForUser = true;
    succeeded(explorer::planNamespaceAction(NamespaceAction::AlwaysAvailableOffline, network, {}, {}, &plan), "Plan user pin toggle without changing shared cache");
    require(plan.checked && plan.canonicalVerb == L"IOfflineFilesCache.Unpin", "Existing per-user pin was not offered as a checked toggle");
    network.uncPaths = false;
    require(FAILED(explorer::planNamespaceAction(NamespaceAction::MapAsDrive, network, {}, {entry(90,L"Windows.connectNetworkDrive")}, &plan)),
            "Local folder offered target-specific network mapping");
    for (size_t index = 0; index < static_cast<size_t>(NamespaceAction::Count); ++index)
        require(!explorer::namespaceActionLabel(static_cast<NamespaceAction>(index)).empty(), "Namespace action is missing descriptive metadata");
}

void nativeFixtureCapabilitiesAndHeadlessGuard() {
    Fixture fixture;
    const auto imageBefore = read(fixture.image);
    const auto textBefore = read(fixture.text);
    const auto folder = item(fixture.root);
    const auto image = item(fixture.image);
    explorer::NativeNamespaceActions actions;
    explorer::NamespaceTarget target{folder,array(image.Get()),{}};
    succeeded(actions.initialize(nullptr,target), "Initialize namespace planner on owned image fixture");
    require(actions.facts().selectionCount == 1 && actions.facts().physicalFiles && actions.facts().images &&
            fs::equivalent(fs::path(actions.facts().singlePath), fixture.image), "Actual bitmap facts differ from Shell selection");
    std::vector<explorer::ContextMenuEntry> entries;
    succeeded(actions.commandStoreEntries(entries), "Enumerate installed command handlers without invoking UI");
    require(!entries.empty(), "Native CommandStore menu was unexpectedly empty");
    NamespaceInvocationPlan plan;
    succeeded(actions.planCommandStore(L"Windows.properties",&plan), "Plan exact installed generic Windows command");
    require(plan.route == NamespaceInvocationRoute::CommandStoreMenu && plan.commandId > 0 &&
            plan.canonicalVerb == L"Windows.properties", "Generic command did not preserve installed native target");
    succeeded(actions.planInvocation(NamespaceAction::RotateRight,&plan), "Plan real native bitmap rotation without modifying it");
    require(plan.enabled && (plan.canonicalVerb == L"rotate90" || plan.canonicalVerb == L"Windows.rotate90"),
            "Installed Windows bitmap rotation was not routed through its native verb");
    const HRESULT mail = actions.planInvocation(NamespaceAction::Email,&plan);
    if (SUCCEEDED(mail)) {
        require(plan.enabled && (plan.route == NamespaceInvocationRoute::SendToMailRecipient ||
                                plan.route == NamespaceInvocationRoute::SelectionMenu ||
                                plan.route == NamespaceInvocationRoute::CommandStoreMenu), "Native email capability returned a placeholder route");
        if (plan.route == NamespaceInvocationRoute::SendToMailRecipient)
            require(actions.facts().mailRecipientAvailable && fs::exists(plan.target) &&
                    _wcsicmp(fs::path(plan.target).extension().c_str(),L".MAPIMail") == 0,
                    "Email planner invented a SendTo recipient");
    } else require(!actions.facts().mailRecipientAvailable, "Available native mail recipient was not exposed");
    const HRESULT cast = actions.planInvocation(NamespaceAction::CastToDevice,&plan);
    if (SUCCEEDED(cast) && plan.route == NamespaceInvocationRoute::RegisteredHandlerMenu)
        require(actions.facts().castHandlerAvailable && plan.commandId &&
                plan.submenu == actions.facts().castHandlerSubmenu,
                "Registered PlayTo handler menu was not retained");
    for (size_t index = 0; index < static_cast<size_t>(NamespaceAction::Count); ++index)
        require(actions.invoke(static_cast<NamespaceAction>(index),true) == E_ACCESSDENIED,
                "Headless namespace action reached a native UI/destructive handler");
    require(actions.invokeCommandStore(L"Windows.runas",true) == E_ACCESSDENIED,
            "Headless generic command reached native activation");
    require(actions.invokeCommandStore(L"Windows.properties",false) == E_ACCESSDENIED,
            "Missing visible owner bypassed the UI guard");
    require(actions.planCommandStore(L"Windows.\\malformed",&plan) == E_INVALIDARG &&
            actions.planCommandStore(std::wstring_view(L"Windows.X\0Y",11),&plan) == E_INVALIDARG,
            "Malformed registry command accepted");
    require(FAILED(actions.planCommandStore(L"Windows.ThisCommandIsNotInstalled",&plan)), "Missing registered native command was invented");
    HRESULT otherThread = S_OK;
    std::thread worker([&] { NamespaceInvocationPlan ignored; otherThread = actions.planInvocation(NamespaceAction::RotateRight,&ignored); });
    worker.join();
    require(otherThread == RPC_E_WRONG_THREAD, "STA-owned native menus were accessed on another thread");
    explorer::NamespaceCommandMetadata metadata;
    metadata.label=L"preserved";
    std::thread metadataWorker([&] { otherThread=actions.commandMetadata(L"Windows.copy",&metadata); });
    metadataWorker.join();
    require(otherThread==RPC_E_WRONG_THREAD && metadata.label==L"preserved",
            "View-owned native presentation crossed apartments or changed failure output");
    require(read(fixture.image) == imageBefore && read(fixture.text) == textBefore && fs::exists(fixture.program) && fs::exists(fixture.disc),
            "Native capability planning changed owned fixture content");
    actions.reset();
    require(actions.planInvocation(NamespaceAction::Email,&plan) == E_UNEXPECTED, "Reset namespace planner retained a stale target");
    require(actions.commandMetadata(L"Windows.copy",&metadata)==E_UNEXPECTED && metadata.label==L"preserved",
            "Reset presentation query used a stale selection/site or changed output");
    target.selection.Reset();
    succeeded(actions.initialize(nullptr,target), "Initialize implied current-folder target");
    require(actions.facts().selectionCount == 0 && actions.facts().physicalFolders && !actions.facts().physicalFiles,
            "Current-folder implication became an explicit file selection");
    require(FAILED(actions.planInvocation(NamespaceAction::Email,&plan)), "Current folder was implicitly attached to mail");
    succeeded(actions.refresh(), "Refresh native capability state after operations");
    require(actions.facts().selectionCount == 0 && actions.facts().physicalFolders, "State refresh lost original target scope");
}

void metadataAndReadOnlyDriveEnumeration() {
    explorer::NamespaceCommandMetadata metadata;
    succeeded(explorer::namespaceCommandMetadata(L"Windows.copy",&metadata), "Read installed Windows copy label/icon resources");
    require(metadata.command == L"Windows.copy" && !metadata.label.empty() && !metadata.icon.empty(), "Native command metadata lost label or icon");
    metadata.label = L"preserved";
    require(explorer::namespaceCommandMetadata(L"..\\Windows.copy",&metadata) == E_INVALIDARG && metadata.label == L"preserved",
            "Invalid command metadata lookup changed output");
    require(explorer::namespaceCommandMetadata(L"Windows.copy",nullptr) == E_POINTER, "Null native metadata output accepted");
    const auto root = fs::temp_directory_path().root_path();
    const auto drive = item(root);
    ComPtr<IShellItem> computer;
    succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(&computer)), "Get native Computer namespace without opening a view");
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{computer,array(drive.Get()),{}}), "Initialize drive capability planner without touching a device");
    require(actions.facts().driveRoot && fs::equivalent(fs::path(actions.facts().singlePath), root), "Drive target was inferred from the wrong folder");
    NamespaceInvocationPlan plan;
    const HRESULT hr = actions.planInvocation(NamespaceAction::FormatDrive,&plan);
    if (actions.facts().driveType == DRIVE_FIXED || actions.facts().driveType == DRIVE_REMOVABLE || actions.facts().driveType == DRIVE_RAMDISK) {
        succeeded(hr,"Enumerate native format capability without opening the format dialog");
        require(plan.commandId && plan.nativeConfirmation && plan.target == root.native(), "Drive formatting plan lost its exact target");
    }
    require(actions.invoke(NamespaceAction::FormatDrive,true) == E_ACCESSDENIED &&
            actions.invoke(NamespaceAction::EjectDrive,true) == E_ACCESSDENIED &&
            actions.invoke(NamespaceAction::CleanUpDrive,true) == E_ACCESSDENIED,
            "Headless drive actions crossed the execution guard");
    succeeded(actions.commandMetadata(L"Windows.Defragment",&metadata), "Resolve native handler-derived optimization label");
    require(!metadata.label.empty() && !metadata.icon.empty(), "Native optimization title/icon is missing");
    for (const auto key : {L"Windows.Autoplay",L"Windows.FinishBurn",L"Windows.EraseDisc"}) {
        succeeded(actions.commandMetadata(key,&metadata),"Read actual installed Drive Media label/icon without invoking a device");
        require(!metadata.label.empty() && !metadata.icon.empty(),"Drive Media native presentation is missing");
        explorer::NamespaceCommandState state;
        const auto status=actions.queryCommandState(key,&state);
        require(SUCCEEDED(status)||status==E_PENDING,"Registered Drive Media state handler could not be queried read-only");
        if(SUCCEEDED(status))require(state.handler!=CLSID_NULL&&state.initialized&&!state.explorerCommand,
                "Drive Media state did not use its exact initialized native state-only handler");
        require(actions.invokeCommandStore(key,true)==E_ACCESSDENIED,"Drive Media command crossed its headless device guard");
    }
}

void nativeViewGalleryResources() {
    std::vector<explorer::NamespaceSubcommandMetadata> gallery;
    succeeded(explorer::namespaceCommandChildren(L"Windows.IconSize",nullptr,nullptr,&gallery),
              "Enumerate native Windows View gallery resources without changing a view");
    require(gallery.size() == 8, "Windows View gallery does not contain all eight native modes");
    const std::array<const wchar_t*,8> icons{L"shell32.dll,-63001",L"shell32.dll,-63008",L"shell32.dll,-63009",L"shell32.dll,-63010",
                                           L"shell32.dll,-63000",L"shell32.dll,-62998",L"shell32.dll,-62999",L"shell32.dll,-63011"};
    for (size_t index = 0; index < gallery.size(); ++index) {
        require(!gallery[index].label.empty() && !gallery[index].icon.empty(), "Native View mode title/icon was replaced with a placeholder");
        require(gallery[index].icon == icons[index], "Native View mode order or resource differs from installed Windows 10 gallery");
    }
    const auto first = gallery[0].label;
    require(explorer::namespaceCommandChildren(L"Windows.Bad\\Command",nullptr,nullptr,&gallery) == E_INVALIDARG && gallery[0].label == first,
            "Invalid child resource request changed output");
    require(explorer::namespaceCommandChildren(L"Windows.IconSize",nullptr,nullptr,nullptr) == E_POINTER,
            "Null native gallery output accepted");
}

class AggregateSelectionMenu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IContextMenu,IObjectWithSite> {
public:
    unsigned queries=0,invocations=0,attachments=0,detachments=0;
    IUnknown* expectedSite=nullptr;
    bool exactSite=false;
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu,UINT position,UINT first,UINT last,UINT) override {
        ++queries;exactSite=site_.Get()==expectedSite;
        if(last<first+2)return E_INVALIDARG;
        if(!InsertMenuW(menu,position,MF_BYPOSITION|MF_STRING,first+2,L"Owned native provider leaf"))
            return HRESULT_FROM_WIN32(GetLastError());
        return MAKE_HRESULT(SEVERITY_SUCCESS,0,3);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override { ++invocations;return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR ordinal,UINT flags,UINT*,LPSTR text,UINT capacity) override {
        if(ordinal!=2||flags!=GCS_VERBW)return E_NOTIMPL;
        return wcscpy_s(reinterpret_cast<wchar_t*>(text),capacity,L"Windows.burn")?E_FAIL:S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if(site)++attachments;else++detachments;site_=site;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        return site_?site_->QueryInterface(iid,result):E_NOINTERFACE;
    }
private:
    ComPtr<IUnknown> site_;
};

class AggregateSelection final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray> {
public:
    DWORD count=100001;
    SFGAOF prefix=SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK;
    SFGAOF tail=SFGAO_FILESYSTEM;
    DWORD inspected=0;
    unsigned itemReads=0,attributeCalls=0,binds=0;
    SIATTRIBFLAGS requested=SIATTRIBFLAGS_OR;
    SFGAOF requestedMask=0;
    HRESULT attributeStatus=S_FALSE;
    ComPtr<AggregateSelectionMenu> menu=Microsoft::WRL::Make<AggregateSelectionMenu>();
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID handler,REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;++binds;
        return handler==BHID_SFUIObject?menu->QueryInterface(iid,result):E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS,REFIID,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS flags,SFGAOF mask,SFGAOF* result) override {
        if(!result)return E_POINTER;++attributeCalls;requested=flags;requestedMask=mask;
        if(FAILED(attributeStatus))return attributeStatus;
        if(flags!=static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS))return E_INVALIDARG;
        SFGAOF combined=mask;inspected=0;
        for(DWORD index=0;index<count;++index){combined&=index==count-1?tail:prefix;++inspected;}
        *result=combined;return attributeStatus;
    }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* result) override { if(!result)return E_POINTER;*result=count;return S_OK; }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD,IShellItem** result) override {
        ++itemReads;if(!result)return E_POINTER;*result=nullptr;return E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** result) override {
        if(!result)return E_POINTER;*result=nullptr;return E_NOTIMPL;
    }
};

void aggregateLargeSelectionAndProviderGuards() {
    Fixture fixture;const auto folder=item(fixture.root);
    const auto textBefore=read(fixture.text),imageBefore=read(fixture.image);
    const auto clipboard=GetClipboardSequenceNumber();
    const auto selected=Microsoft::WRL::Make<AggregateSelection>();
    ComPtr<IShellItemArray> full;full=selected;
    ComPtr<IUnknown> site;succeeded(folder.As(&site),"Retain exact mock view-site identity");
    selected->menu->expectedSite=site.Get();
    explorer::NativeNamespaceActions actions;
    succeeded(actions.initialize(nullptr,{folder,full,site}),"Initialize complete 100001-item selection through native aggregate attributes");
    const auto& facts=actions.facts();
    require(facts.selectionCount==100001 && facts.filesystem && facts.nativeAttributes==SFGAO_FILESYSTEM &&
            facts.nativeAttributesStatus==S_FALSE && !facts.detailedTargetsKnown,
            "Large selection lost its final-item counterexample or unknown-metadata distinction");
    require(selected->inspected==selected->count && selected->requested==static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS) &&
            selected->requestedMask==(SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK) && selected->itemReads==0,
            "Large facts used a prefix, eager item reads or approximate attributes");
    require(!facts.physicalFiles&&!facts.physicalFolders&&!facts.images&&!facts.media&&!facts.applications&&
            !facts.uncPaths&&facts.singlePath.empty(),"Unknown optional large-selection details were fabricated");
    NamespaceInvocationPlan plan;
    succeeded(actions.planInvocation(NamespaceAction::BurnToDisc,&plan),"Plan actual full-array provider leaf without per-item metadata");
    require(plan.enabled&&plan.route==NamespaceInvocationRoute::SelectionMenu&&plan.canonicalVerb==L"Windows.burn"&&
            selected->binds==1&&selected->menu->exactSite&&selected->menu->attachments==1&&selected->itemReads==0,
            "Full-array native context provider or its exact site was replaced/truncated");
    for(const auto action:{NamespaceAction::Email,NamespaceAction::Fax,NamespaceAction::Play,NamespaceAction::RotateLeft}) {
        require(explorer::namespaceActionApplicable(action,facts),"Unknown optional details blocked an exact native provider query");
        NamespaceFacts unknown=facts;unknown.mailRecipientAvailable=unknown.offlineActive=true;
        unknown.mailRecipientPath=L"owned installed recipient.MAPIMail";
        require(FAILED(explorer::planNamespaceAction(action,unknown,{},{},&plan)),
                "Unknown large-selection details enabled a recipient/type fallback without native evidence");
    }
    require(actions.invoke(NamespaceAction::BurnToDisc,true)==E_ACCESSDENIED&&
            actions.invoke(NamespaceAction::BurnToDisc,false)==E_ACCESSDENIED&&
            actions.invokeCommandStore(L"Windows.recycle",true)==E_ACCESSDENIED&&selected->menu->invocations==0,
            "Large selection weakened a native activation/mutation guard");
    actions.reset();require(selected->menu->detachments==1,"Large provider site remained attached after reset");
    selected->tail=0;
    succeeded(actions.initialize(nullptr,{folder,full,site}),"Retain native all-items filesystem counterexample at index100000");
    require(!actions.facts().filesystem && actions.facts().nativeAttributes==0 && selected->itemReads==0,
            "A last nonfilesystem item was ignored in native eligibility");
    require(!explorer::namespaceActionApplicable(NamespaceAction::ShareSpecificPeople,actions.facts()),
            "A virtual tail inherited filesystem sharing permission from its prefix");
    selected->tail=selected->prefix;
    succeeded(actions.initialize(nullptr,{folder,full,site}),"Read all-folders/all-links native aggregate truth");
    require((actions.facts().nativeAttributes&(SFGAO_FOLDER|SFGAO_LINK))==(SFGAO_FOLDER|SFGAO_LINK) &&
            !explorer::namespaceActionApplicable(NamespaceAction::Play,actions.facts()),
            "Verified all-folders native selection acquired a file-only context");
    selected->attributeStatus=E_ACCESSDENIED;
    require(actions.refresh()==E_ACCESSDENIED && actions.facts().nativeAttributes==(SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK),
            "Failed native aggregate refresh invented attributes or changed existing facts");
    require(actions.initialize(nullptr,{folder,full,site})==E_ACCESSDENIED&&actions.facts().selectionCount==0,
            "Failed initial aggregate query retained partially initialized targets");
    require(read(fixture.text)==textBefore&&read(fixture.image)==imageBefore&&GetClipboardSequenceNumber()==clipboard,
            "Large-array capability proof changed owned contents or clipboard");
}

void nativeLargeArrayTailCounterexamples() {
    Fixture fixture;const auto folder=item(fixture.root),text=item(fixture.text);
    const auto before=read(fixture.text);const auto clipboard=GetClipboardSequenceNumber();
    struct Identities {
        PIDLIST_ABSOLUTE folder=nullptr,text=nullptr,virtualItem=nullptr,shortcut=nullptr;
        ~Identities(){CoTaskMemFree(folder);CoTaskMemFree(text);CoTaskMemFree(virtualItem);CoTaskMemFree(shortcut);}
    } identities;
    succeeded(SHGetIDListFromObject(folder.Get(),&identities.folder),"Retain owned folder PIDL for full native large array");
    succeeded(SHGetIDListFromObject(text.Get(),&identities.text),"Retain owned file PIDL for native final-item counterexample");
    ComPtr<IShellItem> controlPanel;
    succeeded(SHGetKnownFolderItem(FOLDERID_ControlPanelFolder,KF_FLAG_DONT_VERIFY,nullptr,IID_PPV_ARGS(&controlPanel)),
              "Get static native virtual identity without enumerating user items");
    succeeded(SHGetIDListFromObject(controlPanel.Get(),&identities.virtualItem),"Retain static virtual PIDL for native tail counterexample");
    ComPtr<IShellLinkW> link;succeeded(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),
                                     "Create actual owned shortcut without resolving or launching it");
    succeeded(link->SetPath(fixture.text.c_str()),"Set only the stored owned shortcut target");
    ComPtr<IPersistFile> persistence;succeeded(link.As(&persistence),"Read owned shortcut persistence interface");
    const auto shortcutPath=fixture.root/L"owned shortcut.lnk";
    succeeded(persistence->Save(shortcutPath.c_str(),TRUE),"Save only the owned shortcut identity");
    const auto shortcut=item(shortcutPath);
    succeeded(SHGetIDListFromObject(shortcut.Get(),&identities.shortcut),"Retain real native shortcut PIDL");
    std::vector<PCIDLIST_ABSOLUTE> ids(100001,identities.folder);
    explorer::NativeNamespaceActions actions;
    const std::array<std::pair<PCIDLIST_ABSOLUTE,PCIDLIST_ABSOLUTE>,3> examples{{
        {identities.folder,identities.text},{identities.folder,identities.virtualItem},{identities.shortcut,identities.text}}};
    for(const auto& [prefix,tail]:examples) {
        std::fill(ids.begin(),ids.end(),prefix);ids.back()=tail;
        ComPtr<IShellItemArray> selected;
        succeeded(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),ids.data(),&selected),
                  "Construct real native 100001-item array from repeated owned identities");
        DWORD count=0;succeeded(selected->GetCount(&count),"Read exact native large array count");
        require(count==ids.size(),"Native large-array construction changed the original item count");
        SFGAOF expected=0;
        const auto status=selected->GetAttributes(static_cast<SIATTRIBFLAGS>(SIATTRIBFLAGS_AND|SIATTRIBFLAGS_ALLITEMS),
                                                 SFGAO_FILESYSTEM|SFGAO_FOLDER|SFGAO_LINK,&expected);
        succeeded(status,"Compute documented native AND attributes through final counterexample");
        succeeded(actions.initialize(nullptr,{folder,selected,{}}),"Retain real full native large array without item cap");
        require(actions.facts().selectionCount==count&&actions.facts().nativeAttributes==expected&&
                actions.facts().nativeAttributesStatus==status&&!actions.facts().detailedTargetsKnown,
                "Production native aggregate facts differ from complete native array readback");
        std::cout<<"Large native array tailVirtual="<<(tail==identities.virtualItem)<<" prefixLink="<<(prefix==identities.shortcut)
                 <<" attributes=0x"<<std::hex<<expected<<std::dec<<" status="<<status<<'\n';
        require(((expected&SFGAO_FOLDER)!=0)==(tail==identities.virtualItem)&&!(expected&SFGAO_LINK)&&
                (!(expected&SFGAO_FILESYSTEM)==(tail==identities.virtualItem)),
                "Native final file/virtual counterexample was lost behind a repeated-folder prefix");
        ComPtr<IShellItem> retainedTail;succeeded(selected->GetItemAt(count-1,&retainedTail),"Read unchanged exact native last target");
        int order=1;succeeded(retainedTail->Compare(tail==identities.text?text.Get():controlPanel.Get(),SICHINT_CANONICAL,&order),
                              "Compare retained original last-item identity");
        require(order==0,"The native full selection lost its last target identity");
        explorer::NamespaceCommandState facade,direct;
        const auto directStatus=explorer::namespaceCommandState(L"Windows.ShareSpecificUsers",selected.Get(),nullptr,&direct);
        const auto facadeStatus=actions.queryCommandState(L"Windows.ShareSpecificUsers",&facade);
        require(directStatus==facadeStatus&&(!SUCCEEDED(directStatus)||direct.state==facade.state),
                "Large-selection facade changed exact native provider status/state");
        require(actions.invokeCommandStore(L"Windows.ShareSpecificUsers",true)==E_ACCESSDENIED&&
                actions.invokeZip(true)==E_ACCESSDENIED,"Native large-array proof activated recipients or payload handlers");
        actions.reset();
    }
    require(read(fixture.text)==before&&fs::exists(fixture.image)&&GetClipboardSequenceNumber()==clipboard,
            "Native large-array attribute/state proof changed owned files or clipboard");
}
} // namespace

int runNamespaceActionTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"semantic target guards and exact canonical verbs",semanticPlanningAndCanonicalVerbs},
        {"disabled ancestors, ambiguous handlers and native cascades",disabledMenusAndAmbiguousHandlers},
        {"drive and simulated Recycle Bin action planning",destructiveAndOptionalActionPlanning},
        {"sharing, application, network and media routes",targetTypesAndFeatureMatrix},
        {"native fixture capabilities, STA lifetime and headless activation guard",nativeFixtureCapabilitiesAndHeadlessGuard},
        {"Windows command resources and read-only drive enumeration",metadataAndReadOnlyDriveEnumeration},
        {"all eight native localized View gallery titles and icon resources",nativeViewGalleryResources},
        {"full 100001-item aggregate attributes, provider site and activation guards",[]{onPrivateNamespaceDesktop(aggregateLargeSelectionAndProviderGuards);}},
        {"real native 100001-item arrays with final file, link and virtual counterexamples",[]{onPrivateNamespaceDesktop(nativeLargeArrayTailCounterexamples);}}
    };
    unsigned failures = 0;
    for (const auto& [name,test] : tests) {
        try { test(); std::cout << "PASS: Namespace actions: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: Namespace actions: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: Namespace actions: " << name << ": unknown exception\n"; }
    }
    try{succeeded(explorer::drainStaWorkers(10000),"Drain namespace suite workers while creator COM remains initialized");}
    catch(const std::exception& error){++failures;std::cerr<<"FAIL: Namespace actions: final STA drain: "<<error.what()<<'\n';}
    std::cout << tests.size() - failures << '/' << tests.size() << " namespace action groups passed; " << assertions << " assertions\n";
    return static_cast<int>(failures);
}
