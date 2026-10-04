#include "explorer/context_menu.hpp"

#include <shlobj.h>
#include <wrl/implements.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using explorer::NativeContextMenu;
using explorer::ContextMenuEntry;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

unsigned assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

struct HandlerState {
    UINT first = 0;
    UINT last = 0;
    UINT queryFlags = 0;
    HRESULT queryResult = S_OK;
    HRESULT message3Result = S_OK;
    UINT range = 10;
    UINT invokedOffset = UINT_MAX;
    DWORD invokeMask = 0;
    HWND invokeOwner = nullptr;
    POINT invokePoint{};
    unsigned invokes = 0;
    unsigned message2Calls = 0;
    unsigned message3Calls = 0;
    unsigned attached = 0;
    unsigned detached = 0;
    unsigned destroyed = 0;
    bool childPopulated = false;
    bool detachedAfterMenuDestroyed = false;
    HMENU root = nullptr;
    HMENU child = nullptr;

    HRESULT query(HMENU menu, UINT firstId, UINT lastId, UINT flags) {
        first = firstId;
        last = lastId;
        queryFlags = flags;
        root = menu;
        if (FAILED(queryResult)) return queryResult;
        child = CreatePopupMenu();
        if (!child) return E_OUTOFMEMORY;
        AppendMenuW(child, MF_STRING, first + 2, L"Native child");
        MENUITEMINFOW group{sizeof(group)};
        group.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
        group.hSubMenu = child;
        group.wID = first + 6;
        group.dwTypeData = const_cast<wchar_t*>(L"Localized cascade");
        InsertMenuItemW(menu, 0, TRUE, &group);
        AppendMenuW(menu, MF_STRING, first, L"Enabled \u6587\u4EF6");
        AppendMenuW(menu, MF_STRING | MF_DISABLED, first + 1, L"Disabled");
        MENUITEMINFOW separator{sizeof(separator)};
        separator.fMask = MIIM_FTYPE | MIIM_ID;
        separator.fType = MFT_SEPARATOR;
        separator.wID = first + 3;
        InsertMenuItemW(menu, 3, TRUE, &separator);
        AppendMenuW(menu, MF_STRING, 40000, L"Invalid extension ID");
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, range);
    }

    HRESULT command(CMINVOKECOMMANDINFO* basic) {
        const auto* info = reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(basic);
        if (info->cbSize != sizeof(*info) || HIWORD(reinterpret_cast<UINT_PTR>(info->lpVerb)) ||
            HIWORD(reinterpret_cast<UINT_PTR>(info->lpVerbW))) return E_INVALIDARG;
        ++invokes;
        invokedOffset = LOWORD(reinterpret_cast<UINT_PTR>(info->lpVerb));
        invokeMask = info->fMask;
        invokeOwner = info->hwnd;
        invokePoint = info->ptInvoke;
        return S_OK;
    }

    HRESULT string(UINT_PTR offset, UINT flags, LPSTR result, UINT capacity) {
        if (offset == 0 && flags == GCS_VERBW)
            return wcscpy_s(reinterpret_cast<wchar_t*>(result), capacity, L"fixture") ? E_FAIL : S_OK;
        if (offset == 2 && flags == GCS_VERBA)
            return strcpy_s(result, capacity, "legacy") ? E_FAIL : S_OK;
        return E_NOTIMPL;
    }

    void populate(UINT message, WPARAM wParam) {
        if (message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wParam) == child &&
            !childPopulated) {
            AppendMenuW(child, MF_STRING, first + 9, L"Delayed child");
            childPopulated = true;
        }
    }
};

class FakeMenu3 final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                            ChainInterfaces<IContextMenu3, IContextMenu2, IContextMenu>,
                                            IObjectWithSite> {
public:
    explicit FakeMenu3(std::shared_ptr<HandlerState> state) : state_(std::move(state)) {}
    ~FakeMenu3() { ++state_->destroyed; }
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT, UINT first, UINT last, UINT flags) override {
        return state_->query(menu, first, last, flags);
    }
    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override { return state_->command(info); }
    IFACEMETHODIMP GetCommandString(UINT_PTR id, UINT flags, UINT*, LPSTR text, UINT size) override {
        return state_->string(id, flags, text, size);
    }
    IFACEMETHODIMP HandleMenuMsg(UINT message, WPARAM wParam, LPARAM) override {
        ++state_->message2Calls;
        state_->populate(message, wParam);
        return S_OK;
    }
    IFACEMETHODIMP HandleMenuMsg2(UINT message, WPARAM wParam, LPARAM, LRESULT* result) override {
        ++state_->message3Calls;
        if (state_->message3Result == S_OK) {
            state_->populate(message, wParam);
            if (result) *result = message == WM_MENUCHAR ? MAKELRESULT(1, MNC_EXECUTE) : 42;
        }
        return state_->message3Result;
    }
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (site) ++state_->attached;
        else {
            ++state_->detached;
            state_->detachedAfterMenuDestroyed = !IsMenu(state_->root);
        }
        site_ = site;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return site_ ? site_->QueryInterface(iid, result) : E_FAIL;
    }
private:
    std::shared_ptr<HandlerState> state_;
    ComPtr<IUnknown> site_;
};

class FakeMenu2 final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                            ChainInterfaces<IContextMenu2, IContextMenu>> {
public:
    explicit FakeMenu2(std::shared_ptr<HandlerState> state) : state_(std::move(state)) {}
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT, UINT first, UINT last, UINT flags) override {
        return state_->query(menu, first, last, flags);
    }
    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override { return state_->command(info); }
    IFACEMETHODIMP GetCommandString(UINT_PTR id, UINT flags, UINT*, LPSTR text, UINT size) override {
        return state_->string(id, flags, text, size);
    }
    IFACEMETHODIMP HandleMenuMsg(UINT message, WPARAM wParam, LPARAM) override {
        ++state_->message2Calls;
        state_->populate(message, wParam);
        return S_OK;
    }
private:
    std::shared_ptr<HandlerState> state_;
};

void snapshotAndCommandSafety() {
    auto state = std::make_shared<HandlerState>();
    auto fake = Make<FakeMenu3>(state);
    NativeContextMenu menu;
    require(SUCCEEDED(menu.create(nullptr, fake.Get())), "Cannot host fake native menu");
    require(state->first == 1 && state->last == 0x7fff, "Query command bounds changed");
    require((state->queryFlags & CMF_SYNCCASCADEMENU) != 0, "Submenus must be queried synchronously");
    require(menu.commandCount() == state->range, "Query range was not retained");
    std::vector<ContextMenuEntry> entries;
    require(SUCCEEDED(menu.enumerate(entries, false)), "Unpopulated snapshot failed");
    require(entries.size() == 5 && entries[0].submenu && !entries[1].submenu &&
            entries[0].children.size() == 1, "Unpopulated snapshot changed");
    require(entries[1].label == L"Enabled \u6587\u4EF6", "Unicode labels were lost");
    require(entries[1].canonicalVerb == L"fixture", "Unicode canonical verb missing");
    require(entries[0].children[0].canonicalVerb == L"legacy", "ANSI canonical verb fallback failed");
    require(!entries[2].enabled() && entries[3].separator(), "Native menu capabilities were lost");
    require(SUCCEEDED(menu.enumerate(entries)) && entries[0].children.size() == 2,
            "Delayed native submenu was not initialized headlessly");
    require(SUCCEEDED(menu.enumerate(entries)) && entries[0].children.size() == 2,
            "Repeated snapshot duplicated native submenu children");
    require(menu.invoke(0) == E_INVALIDARG, "Cancellation invoked a command");
    require(menu.invoke(2) == E_INVALIDARG, "Disabled command was invoked");
    require(menu.invoke(4) == E_INVALIDARG, "Separator was invoked");
    require(menu.invoke(7) == E_INVALIDARG, "Cascade header was invoked");
    require(menu.invoke(5) == E_INVALIDARG, "Unused ordinal hole was invoked");
    require(menu.invoke(40000) == E_INVALIDARG, "Extension ID outside assigned range was invoked");
    require(menu.invoke(0x10001) == E_INVALIDARG, "Command ID was truncated to 16 bits");
    require(state->invokes == 0, "Rejected commands reached the handler");
    require(SUCCEEDED(menu.invoke(10, POINT{13, 17}, true, true)), "Delayed leaf could not be invoked");
    require(state->invokedOffset == 9 && state->invokePoint.x == 13 && state->invokePoint.y == 17,
            "Invoke did not use the actual selected command offset and screen position");
    require((state->invokeMask & (CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
                                CMIC_MASK_CONTROL_DOWN | CMIC_MASK_SHIFT_DOWN)) ==
                               (CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
                                CMIC_MASK_CONTROL_DOWN | CMIC_MASK_SHIFT_DOWN),
            "Invoke lost Unicode/modifier information");
    require((state->invokeMask & CMIC_MASK_ASYNCOK) == 0, "Async execution requires a thread reference");
    DeleteMenu(state->child, 10, MF_BYCOMMAND);
    require(menu.invoke(10) == E_INVALIDARG, "Stale removed command was invoked");
    AppendMenuW(state->child, MF_STRING, 1, L"Ambiguous duplicate");
    require(menu.invoke(1) == E_INVALIDARG, "Ambiguous duplicate command ID was invoked");
    DeleteMenu(state->child, 1, MF_BYCOMMAND);
    require(SUCCEEDED(menu.invoke(1)) && state->invokedOffset == 0,
            "Zero ordinal is valid when the selected ID equals idCmdFirst");
}

void readOnlyLeafStateContract() {
    auto state = std::make_shared<HandlerState>();
    auto fake = Make<FakeMenu3>(state);
    auto site = Make<FakeMenu2>(std::make_shared<HandlerState>());
    NativeContextMenu menu;
    const UINT requested = CMF_ITEMMENU | CMF_EXTENDEDVERBS | CMF_SYNCCASCADEMENU;
    require(SUCCEEDED(menu.createLeafState(fake.Get(), site.Get(), requested)), "Read-only native leaf menu creation failed");
    require(state->queryFlags == (requested & ~CMF_SYNCCASCADEMENU),
            "Leaf-state creation changed normal flags or requested synchronous cascades");
    require(state->attached == 1 && menu.commandCount() == state->range,
            "Leaf-state query lost exact site attachment or native command bounds");
    std::vector<ContextMenuEntry> entries;
    require(SUCCEEDED(menu.enumerate(entries, false)) && entries.size() == 5 &&
            entries[1].canonicalVerb == L"fixture" && !entries[2].enabled(),
            "Read-only query changed actual canonical leaf/disabled states");
    require(!state->childPopulated && state->message2Calls == 0 && state->message3Calls == 0,
            "Leaf-state enumeration initialized a delayed native cascade");
    require(menu.enumerate(entries, true) == E_ACCESSDENIED && entries.empty(),
            "Read-only leaf menu accepted delayed population");
    LRESULT result = 71;
    require(!menu.handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(state->child), 0, result) && result == 71,
            "Read-only menu forwarded a native cascade message or changed rejected output");
    require(menu.invoke(menu.firstCommand()) == E_ACCESSDENIED && state->invokes == 0,
            "Read-only state query invoked an enabled native command");
    menu.reset();
    require(state->detached == 1 && state->detachedAfterMenuDestroyed,
            "Leaf-state reset released native data/site in the wrong order");
    require(SUCCEEDED(menu.create(nullptr, fake.Get(), site.Get(), CMF_EXTENDEDVERBS)) &&
            (state->queryFlags & CMF_SYNCCASCADEMENU),
            "Read-only creation changed later normal popup cascade flags");
    require(SUCCEEDED(menu.enumerate(entries, true)) && state->childPopulated,
            "Reset did not restore normal native delayed submenu behavior");
    menu.reset();
    state->queryResult = E_ACCESSDENIED;
    require(menu.createLeafState(fake.Get(), site.Get()) == E_ACCESSDENIED && !menu.menu() &&
            state->attached == state->detached, "Failed leaf-state query leaked menu/site or changed failure");
    require(menu.createLeafState(nullptr) == E_INVALIDARG, "Leaf-state query accepted a null handler");
}

void messageRouting() {
    auto state = std::make_shared<HandlerState>();
    auto fake = Make<FakeMenu3>(state);
    NativeContextMenu menu;
    require(SUCCEEDED(menu.create(nullptr, fake.Get())), "Cannot create message routing fixture");
    require(menu.supportsMenuMessages() && menu.supportsMenuCharacters(), "CM3 capabilities missing");
    LRESULT result = -1;
    require(menu.handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(state->child), 0, result) &&
            result == 42 && state->message3Calls == 1 && state->message2Calls == 0,
            "CM3 must route before CM2 and preserve its result");
    require(menu.handleMessage(WM_MENUCHAR, 0, reinterpret_cast<LPARAM>(state->child), result) &&
            result == MAKELRESULT(1, MNC_EXECUTE), "WM_MENUCHAR native result lost");
    HMENU foreign = CreatePopupMenu();
    require(!menu.handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(foreign), 0, result),
            "Foreign popup message reached the Shell handler");
    require(!menu.handleMessage(WM_MENUCHAR, 0, reinterpret_cast<LPARAM>(foreign), result),
            "Foreign menu character message reached the Shell handler");
    DestroyMenu(foreign);
    require(!menu.handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu.menu()),
                               MAKELPARAM(0, TRUE), result), "System menu reached the Shell handler");
    DRAWITEMSTRUCT draw{};
    draw.CtlType = ODT_BUTTON;
    draw.hwndItem = reinterpret_cast<HWND>(menu.menu());
    require(!menu.handleMessage(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw), result),
            "Owner-drawn button was sent to the Shell handler");
    draw.CtlType = ODT_MENU;
    require(menu.handleMessage(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw), result),
            "Native menu drawing was not routed");
    require(!menu.handleMessage(WM_DRAWITEM, 1, reinterpret_cast<LPARAM>(&draw), result),
            "Control drawing was sent to the Shell handler");
    MEASUREITEMSTRUCT measure{};
    measure.CtlType = ODT_LISTBOX;
    require(!menu.handleMessage(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure), result),
            "Listbox measuring was sent to the Shell handler");
    measure.CtlType = ODT_MENU;
    require(menu.handleMessage(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure), result),
            "Native menu measuring was not routed");
    require(!menu.handleMessage(WM_COMMAND, 1, 0, result), "WM_COMMAND must remain with host");
    state->message3Result = S_FALSE;
    require(menu.handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu.menu()), 0, result) &&
            result == 0 && state->message2Calls == 1, "Unhandled CM3 initialization needs CM2 fallback");
    require(!menu.handleMessage(WM_MENUCHAR, 0, reinterpret_cast<LPARAM>(menu.menu()), result) &&
            state->message2Calls == 1, "CM2 cannot provide a WM_MENUCHAR result");
}

void contextMenu2Fallback() {
    auto state = std::make_shared<HandlerState>();
    auto fake = Make<FakeMenu2>(state);
    NativeContextMenu menu;
    require(SUCCEEDED(menu.create(nullptr, fake.Get())), "Cannot create CM2 fixture");
    require(menu.supportsMenuMessages() && !menu.supportsMenuCharacters(), "CM2 capabilities wrong");
    LRESULT result = -1;
    MEASUREITEMSTRUCT measure{};
    measure.CtlType = ODT_MENU;
    require(menu.handleMessage(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure), result) &&
            result == TRUE, "CM2 measuring must return TRUE from owner");
    require(!menu.handleMessage(WM_MENUCHAR, 0, reinterpret_cast<LPARAM>(menu.menu()), result),
            "CM2 cannot handle menu character result");
    std::vector<ContextMenuEntry> entries;
    require(SUCCEEDED(menu.enumerate(entries)) && state->childPopulated,
            "CM2 delayed population must work headlessly");
}

void lifetimeAndFailures() {
    auto state = std::make_shared<HandlerState>();
    auto fake = Make<FakeMenu3>(state);
    auto site = Make<FakeMenu2>(std::make_shared<HandlerState>());
    NativeContextMenu menu;
    require(SUCCEEDED(menu.create(nullptr, fake.Get(), site.Get())), "Sited menu creation failed");
    require(state->attached == 1, "Shell handler site was not attached");
    HMENU root = menu.menu();
    HMENU child = state->child;
    fake.Reset();
    require(state->destroyed == 0, "Handler was released before its menu");
    menu.reset();
    require(!IsMenu(root) && !IsMenu(child), "Root menu did not own complete submenu lifetime");
    require(state->detached == 1 && state->detachedAfterMenuDestroyed,
            "Site was not detached after menu data lifetime ended");
    require(state->destroyed == 1, "Menu retained handler/site references after reset");
    require(menu.menu() == nullptr && menu.popup() == nullptr && menu.commandCount() == 0,
            "Reset retained stale command state");
    menu.reset();
    require(state->detached == 1, "Repeated reset detached site twice");
    require(menu.invoke(1) == E_UNEXPECTED, "Empty menu invoked a command");
    std::vector<ContextMenuEntry> entries;
    require(menu.enumerate(entries) == E_UNEXPECTED && entries.empty(), "Empty menu snapshot succeeded");
    require(menu.create(nullptr, nullptr) == E_INVALIDARG, "Null handler accepted");
    require(menu.createBackground(nullptr, nullptr) == E_INVALIDARG, "Null background accepted");
    require(menu.createSelection(nullptr, nullptr) == E_INVALIDARG, "Null selection accepted");
    require(menu.createNewItems(nullptr, nullptr) == E_INVALIDARG, "Null New destination accepted");
    state = std::make_shared<HandlerState>();
    state->queryResult = E_ACCESSDENIED;
    fake = Make<FakeMenu3>(state);
    require(menu.create(nullptr, fake.Get(), site.Get()) == E_ACCESSDENIED && menu.menu() == nullptr,
            "Query failure left live menu state");
    require(state->detached == 1 && !IsMenu(state->root), "Query failure leaked menu/site");
    state->queryResult = S_OK;
    state->range = 0x8000;
    require(menu.create(nullptr, fake.Get()) == E_UNEXPECTED && menu.menu() == nullptr,
            "Handler command range exceeded assigned bounds");
}

void apartmentSafety() {
    auto state = std::make_shared<HandlerState>();
    auto fake = Make<FakeMenu3>(state);
    NativeContextMenu menu;
    require(SUCCEEDED(menu.create(nullptr, fake.Get())), "Cannot create apartment fixture");
    HRESULT enumeration = S_OK;
    HRESULT invocation = S_OK;
    bool routed = true;
    std::thread worker([&] {
        std::vector<ContextMenuEntry> entries;
        enumeration = menu.enumerate(entries);
        invocation = menu.invoke(1);
        LRESULT result = 0;
        routed = menu.handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu.menu()), 0, result);
    });
    worker.join();
    require(enumeration == RPC_E_WRONG_THREAD && invocation == RPC_E_WRONG_THREAD && !routed,
            "Shell menu crossed its owning STA");
    require(state->invokes == 0 && state->message3Calls == 0, "Foreign thread reached native handler");
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            path = base / (L"WindowsExplorer-context-menu-tests-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) return;
            if (error) throw std::runtime_error("Cannot create isolated menu fixture");
        }
        throw std::runtime_error("Cannot allocate isolated menu fixture");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct HiddenOwner {
    HWND window = CreateWindowExW(0, L"STATIC", L"Headless context menu owner",
                                  WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    ~HiddenOwner() { if (window) DestroyWindow(window); }
};

bool processHasVisibleWindow() {
    bool visible = false;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window)) {
            *reinterpret_cast<bool*>(parameter) = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
    return visible;
}

unsigned enabledLeaves(const std::vector<ContextMenuEntry>& entries, UINT first, UINT count) {
    unsigned leaves = 0;
    for (const auto& entry : entries) {
        if (entry.submenu) leaves += enabledLeaves(entry.children, first, count);
        else if (!entry.separator() && entry.enabled() && entry.id >= first && entry.id - first < count)
            ++leaves;
    }
    return leaves;
}

void nativeMenusWithoutDisplayOrInvocation() {
    TemporaryDirectory temporary;
    HiddenOwner owner;
    require(owner.window != nullptr && !IsWindowVisible(owner.window), "Fixture owner must stay hidden");
    require(!processHasVisibleWindow(), "Tests have a visible process window before Shell enumeration");
    ComPtr<IShellItem> folder;
    require(SUCCEEDED(SHCreateItemFromParsingName(temporary.path.c_str(), nullptr,
                                                 IID_PPV_ARGS(&folder))), "Cannot bind menu fixture folder");
    NativeContextMenu background;
    require(SUCCEEDED(background.createBackground(owner.window, folder.Get())),
            "Cannot obtain native folder-background context menu");
    std::vector<ContextMenuEntry> entries;
    require(SUCCEEDED(background.enumerate(entries)) && !entries.empty(),
            "Native background capabilities could not be enumerated headlessly");
    NativeContextMenu newItems;
    require(SUCCEEDED(newItems.createNewItems(owner.window, folder.Get())),
            "Cannot obtain native registered New item menu");
    require(newItems.popup() != newItems.menu(), "New menu must expose its borrowed native cascade");
    require(SUCCEEDED(newItems.enumerate(entries)) &&
            enabledLeaves(entries, newItems.firstCommand(), newItems.commandCount()) >= 2,
            "Native New menu is missing Folder/Shortcut or cannot populate headlessly");
    std::wcout << L"      Registered New item capabilities:";
    for (const auto& entry : entries) if (!entry.separator()) std::wcout << L" [" << entry.label << L"]";
    std::wcout << L'\n';
    require(!processHasVisibleWindow(), "Shell enumeration showed a visible process window");
    require(std::filesystem::is_empty(temporary.path), "Enumeration mutated the owned fixture");
    // A valid ZIP can expose SFGAO_FOLDER while remaining a regular file. Reject
    // it before creating a filesystem ShellNew handler, without invoking one.
    const auto archivePath = temporary.path / L"fixture archive.zip";
    {
        const unsigned char emptyZip[22]{0x50, 0x4b, 0x05, 0x06};
        std::ofstream archive(archivePath, std::ios::binary);
        archive.write(reinterpret_cast<const char*>(emptyZip), sizeof(emptyZip));
        require(archive.good(), "Cannot write owned ZIP menu fixture");
    }
    ComPtr<IShellItem> archive;
    require(SUCCEEDED(SHCreateItemFromParsingName(archivePath.c_str(), nullptr,
                                                 IID_PPV_ARGS(&archive))), "Cannot bind ZIP menu fixture");
    SFGAOF archiveAttributes = 0;
    require(SUCCEEDED(archive->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &archiveAttributes)),
            "Cannot read native ZIP menu attributes");
    const DWORD archiveFileAttributes = GetFileAttributesW(archivePath.c_str());
    require((archiveAttributes & SFGAO_FILESYSTEM) && archiveFileAttributes != INVALID_FILE_ATTRIBUTES &&
            !(archiveFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "ZIP menu fixture must be a physical file");
    const HMENU previousNewMenu = newItems.menu();
    const HRESULT zipMenu = newItems.createNewItems(owner.window, archive.Get());
    const HRESULT expectedZipError = HRESULT_FROM_WIN32(
        (archiveAttributes & SFGAO_FOLDER) ? ERROR_DIRECTORY : ERROR_NOT_SUPPORTED);
    require(zipMenu == expectedZipError, "Filesystem New menu accepted a ZIP namespace destination");
    require(!newItems.menu() && !newItems.popup() && newItems.commandCount() == 0 && !IsMenu(previousNewMenu),
            "Rejected ZIP destination retained the previous New menu");
    require(std::filesystem::file_size(archivePath) == 22, "ZIP destination validation mutated the fixture");
    // No native InvokeCommand calls here: registered ShellNew Command handlers
    // may launch wizards/editors, and have no documented guaranteed-silent mode.
    newItems.reset();
    background.reset();
    const auto filePath = temporary.path / L"fixture.txt";
    { std::ofstream file(filePath); file << "owned menu fixture"; }
    ComPtr<IShellItem> file;
    require(SUCCEEDED(SHCreateItemFromParsingName(filePath.c_str(), nullptr,
                                                 IID_PPV_ARGS(&file))), "Cannot bind selection fixture");
    require(FAILED(newItems.createNewItems(owner.window, file.Get())), "New menu accepted a regular file");
    ComPtr<IShellItemArray> selection;
    require(SUCCEEDED(SHCreateShellItemArrayFromShellItem(file.Get(), IID_PPV_ARGS(&selection))),
            "Cannot create native selection fixture");
    NativeContextMenu itemMenu;
    require(SUCCEEDED(itemMenu.createSelection(owner.window, selection.Get())),
            "Cannot obtain native item menu");
    require(SUCCEEDED(itemMenu.enumerate(entries)) && !entries.empty(),
            "Native item/Open-with capabilities could not be enumerated headlessly");
    require(!processHasVisibleWindow(), "Item enumeration showed a visible process window");
    require(std::filesystem::file_size(filePath) == 18, "Item enumeration mutated the fixture");
}
} // namespace

int runContextMenuTests() {
    assertions = 0;
    const HRESULT apartment = OleInitialize(nullptr);
    if (FAILED(apartment)) {
        std::cerr << "FAIL native context menu STA initialization\n";
        return 1;
    }
    int failures = 0;
    const std::pair<const char*, std::function<void()>> cases[] = {
        {"Native context menu snapshot and command validation", snapshotAndCommandSafety},
        {"Read-only native leaf state flags, lifetime and activation guards", readOnlyLeafStateContract},
        {"Native context menu CM3 owner message routing", messageRouting},
        {"Native context menu CM2 fallback", contextMenu2Fallback},
        {"Native context menu and site lifetime/failures", lifetimeAndFailures},
        {"Native context menu apartment safety", apartmentSafety},
        {"Native context menus headless capability enumeration", nativeMenusWithoutDisplayOrInvocation}
    };
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL " << name << ": unexpected exception\n";
        }
    }
    OleUninitialize();
    std::cout << "Context menu assertions: " << assertions << '\n';
    return failures;
}

#ifdef EXPLORER_CONTEXT_MENU_TEST_STANDALONE
int main() { return runContextMenuTests(); }
#endif
