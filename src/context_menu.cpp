#include "explorer/context_menu.hpp"

#include <shlobj.h>
#include <array>
#include <memory>
#include <new>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned maximumDepth = 16;
constexpr unsigned maximumEntries = 4096;

HRESULT menuError() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

struct PidlDeleter {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};

bool containsMenu(HMENU root, HMENU candidate, unsigned depth, unsigned& budget) noexcept {
    if (!root || !candidate || depth > maximumDepth || !budget) return false;
    if (root == candidate) return true;
    const int count = GetMenuItemCount(root);
    for (int i = 0; i < count && budget; ++i) {
        --budget;
        if (containsMenu(GetSubMenu(root, i), candidate, depth + 1, budget)) return true;
    }
    return false;
}

void findCommand(HMENU root, UINT id, unsigned depth, unsigned& budget,
                 unsigned& matches, bool& enabled) noexcept {
    if (!root || depth > maximumDepth || !budget) return;
    const int count = GetMenuItemCount(root);
    for (int i = 0; i < count && budget; ++i) {
        --budget;
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(root, static_cast<UINT>(i), TRUE, &info)) continue;
        if (info.hSubMenu) {
            findCommand(info.hSubMenu, id, depth + 1, budget, matches, enabled);
        } else if (!(info.fType & MFT_SEPARATOR) && info.wID == id) {
            ++matches;
            enabled = (info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
        }
    }
}
} // namespace

NativeContextMenu::~NativeContextMenu() { reset(); }

void NativeContextMenu::reset() noexcept {
    // The handler owns owner-draw item data, so retain its interfaces until the
    // complete root menu (including borrowed submenus) has been destroyed.
    if (menu_) DestroyMenu(menu_);
    menu_ = popup_ = nullptr;
    if (siteAttached_ && objectWithSite_) objectWithSite_->SetSite(nullptr);
    siteAttached_ = false;
    leafStateOnly_ = false;
    objectWithSite_.Reset();
    context3_.Reset();
    context2_.Reset();
    context_.Reset();
    owner_ = nullptr;
    thread_ = 0;
    commandCount_ = 0;
}

HRESULT NativeContextMenu::create(HWND owner, IContextMenu* context, IUnknown* site,
                                  UINT flags) {
    return createImpl(owner, context, site, flags, false);
}

HRESULT NativeContextMenu::createLeafState(IContextMenu* context, IUnknown* site, UINT flags) {
    return createImpl(nullptr, context, site, flags, true);
}

HRESULT NativeContextMenu::createImpl(HWND owner, IContextMenu* context, IUnknown* site,
                                     UINT flags, bool leafStateOnly) {
    // Retain inputs first: a caller may rebuild using this object's current menu.
    ComPtr<IContextMenu> retained = context;
    ComPtr<IUnknown> retainedSite = site;
    reset();
    if (!retained) return E_INVALIDARG;
    owner_ = owner;
    thread_ = GetCurrentThreadId();
    leafStateOnly_ = leafStateOnly;
    context_ = retained;
    context_.As(&context2_);
    context_.As(&context3_);
    context_.As(&objectWithSite_);
    if (retainedSite && objectWithSite_) {
        const HRESULT hr = objectWithSite_->SetSite(retainedSite.Get());
        if (FAILED(hr)) { reset(); return hr; }
        siteAttached_ = true;
    }
    menu_ = CreatePopupMenu();
    if (!menu_) { const HRESULT hr = menuError(); reset(); return hr; }
    popup_ = menu_;
    // Real popups need populated cascades. A read-only leaf-state worker never
    // opens those cascades, so avoid requesting their synchronous enumeration.
    const UINT nativeFlags = leafStateOnly ? flags & ~CMF_SYNCCASCADEMENU : flags | CMF_SYNCCASCADEMENU;
    const HRESULT hr = context_->QueryContextMenu(menu_, 0, firstCommand_, lastCommand_,
                                                 nativeFlags);
    if (FAILED(hr)) { reset(); return hr; }
    commandCount_ = HRESULT_CODE(hr);
    if (commandCount_ > lastCommand_ - firstCommand_ + 1) {
        reset();
        return E_UNEXPECTED;
    }
    return S_OK;
}

HRESULT NativeContextMenu::createBackground(HWND owner, IShellItem* folder, IUnknown* site,
                                            UINT flags) {
    reset();
    if (!folder) return E_INVALIDARG;
    ComPtr<IShellFolder> shellFolder;
    HRESULT hr = folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&shellFolder));
    if (FAILED(hr)) return hr;
    ComPtr<IContextMenu> context;
    hr = shellFolder->CreateViewObject(owner, IID_PPV_ARGS(&context));
    return FAILED(hr) ? hr : create(owner, context.Get(), site, flags);
}

HRESULT NativeContextMenu::createSelection(HWND owner, IShellItemArray* selection,
                                           IUnknown* site, UINT flags) {
    reset();
    if (!selection) return E_INVALIDARG;
    DWORD count = 0;
    HRESULT hr = selection->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count) return E_INVALIDARG;
    ComPtr<IContextMenu> context;
    hr = selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&context));
    return FAILED(hr) ? hr : create(owner, context.Get(), site, flags | CMF_ITEMMENU);
}

HRESULT NativeContextMenu::createNewItems(HWND owner, IShellItem* folder, IUnknown* site,
                                          UINT flags) {
    reset();
    if (!folder) return E_INVALIDARG;
    SFGAOF attributes = 0;
    HRESULT hr = folder->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
    if (FAILED(hr)) return hr;
    if ((attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    // Compressed files expose Shell folder semantics without being physical
    // directories. Registered ShellNew handlers need a filesystem directory.
    PWSTR rawPath = nullptr;
    hr = folder->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);
    if (FAILED(hr)) return hr;
    if (!path) return E_UNEXPECTED;
    const DWORD physicalAttributes = GetFileAttributesW(path.get());
    if (physicalAttributes == INVALID_FILE_ATTRIBUTES) return menuError();
    if (!(physicalAttributes & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    PIDLIST_ABSOLUTE rawPidl = nullptr;
    hr = SHGetIDListFromObject(folder, &rawPidl);
    std::unique_ptr<ITEMIDLIST_ABSOLUTE, PidlDeleter> pidl(rawPidl);
    if (FAILED(hr)) return hr;
    ComPtr<IContextMenu> context;
    hr = CoCreateInstance(CLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&context));
    if (FAILED(hr)) return hr;
    ComPtr<IShellExtInit> initialize;
    hr = context.As(&initialize);
    if (FAILED(hr)) return hr;
    hr = initialize->Initialize(pidl.get(), nullptr, nullptr);
    if (FAILED(hr)) return hr;
    hr = create(owner, context.Get(), site, flags);
    if (FAILED(hr)) return hr;
    // A dedicated New handler contributes its one cascade. Do not identify it by
    // translated text or assume any of the handler's command offsets.
    const int count = GetMenuItemCount(menu_);
    HMENU cascade = nullptr;
    for (int i = 0; i < count; ++i) {
        if (HMENU child = GetSubMenu(menu_, i)) {
            if (cascade) { reset(); return E_UNEXPECTED; }
            cascade = child;
        }
    }
    if (!cascade) { reset(); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    popup_ = cascade;
    return S_OK;
}

bool NativeContextMenu::ownsMenu(HMENU candidate) const noexcept {
    unsigned budget = maximumEntries;
    return containsMenu(menu_, candidate, 0, budget);
}

bool NativeContextMenu::selectableCommand(UINT id) const noexcept {
    if (id < firstCommand_ || id - firstCommand_ >= commandCount_) return false;
    unsigned budget = maximumEntries;
    unsigned matches = 0;
    bool enabled = false;
    findCommand(menu_, id, 0, budget, matches, enabled);
    return matches == 1 && enabled && budget != 0;
}

std::wstring NativeContextMenu::canonicalVerb(UINT id) const {
    if (id < firstCommand_ || id - firstCommand_ >= commandCount_) return {};
    std::array<wchar_t, 512> wide{};
    HRESULT hr = context_->GetCommandString(id - firstCommand_, GCS_VERBW, nullptr,
                                            reinterpret_cast<LPSTR>(wide.data()),
                                            static_cast<UINT>(wide.size()));
    wide.back() = L'\0';
    if (SUCCEEDED(hr)) return wide.data();
    std::array<char, 512> ansi{};
    hr = context_->GetCommandString(id - firstCommand_, GCS_VERBA, nullptr, ansi.data(),
                                    static_cast<UINT>(ansi.size()));
    ansi.back() = '\0';
    if (FAILED(hr)) return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, ansi.data(), -1, wide.data(),
                                          static_cast<int>(wide.size()));
    return length ? std::wstring(wide.data()) : std::wstring{};
}

HRESULT NativeContextMenu::enumerateMenu(HMENU menu, UINT position, unsigned depth,
                                         unsigned& budget,
                                         std::vector<ContextMenuEntry>& entries,
                                         bool populate) {
    if (depth > maximumDepth || !budget) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (populate) {
        LRESULT ignored = 0;
        handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu),
                      MAKELPARAM(position, FALSE), ignored);
    }
    const int count = GetMenuItemCount(menu);
    if (count < 0) return menuError();
    for (int i = 0; i < count; ++i) {
        if (!budget) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        --budget;
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) return menuError();
        ContextMenuEntry entry;
        entry.id = info.wID;
        entry.type = info.fType;
        entry.state = info.fState;
        entry.submenu = info.hSubMenu != nullptr;
        if (info.cch) {
            if (info.cch > 32767) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            entry.label.resize(static_cast<size_t>(info.cch) + 1);
            info.dwTypeData = entry.label.data();
            ++info.cch;
            if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) return menuError();
            entry.label.resize(info.cch);
        }
        if (!entry.separator()) entry.canonicalVerb = canonicalVerb(entry.id);
        if (info.hSubMenu) {
            const HRESULT hr = enumerateMenu(info.hSubMenu, static_cast<UINT>(i), depth + 1,
                                             budget, entry.children, populate);
            if (FAILED(hr)) return hr;
        }
        entries.push_back(std::move(entry));
    }
    return S_OK;
}

HRESULT NativeContextMenu::enumerate(std::vector<ContextMenuEntry>& entries, bool populate) {
    entries.clear();
    if (!context_ || !menu_) return E_UNEXPECTED;
    if (thread_ != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (leafStateOnly_ && populate) return E_ACCESSDENIED;
    try {
        std::vector<ContextMenuEntry> snapshot;
        unsigned budget = maximumEntries;
        const HRESULT hr = enumerateMenu(popup_, 0, 0, budget, snapshot, populate);
        if (SUCCEEDED(hr)) entries.swap(snapshot);
        return hr;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT NativeContextMenu::invoke(UINT commandId, POINT screenPoint, bool control,
                                 bool shift) {
    if (!context_ || !menu_) return E_UNEXPECTED;
    if (thread_ != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (leafStateOnly_) return E_ACCESSDENIED;
    if (!selectableCommand(commandId)) return E_INVALIDARG;
    CMINVOKECOMMANDINFOEX info{sizeof(info)};
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (control) info.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (shift) info.fMask |= CMIC_MASK_SHIFT_DOWN;
    info.hwnd = owner_;
    info.lpVerb = MAKEINTRESOURCEA(commandId - firstCommand_);
    info.lpVerbW = MAKEINTRESOURCEW(commandId - firstCommand_);
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = screenPoint;
    return context_->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
}

bool NativeContextMenu::handleMessage(UINT message, WPARAM wParam, LPARAM lParam,
                                     LRESULT& result) {
    if (!context_ || !menu_ || thread_ != GetCurrentThreadId() || leafStateOnly_) return false;
    switch (message) {
    case WM_INITMENUPOPUP:
        if (HIWORD(lParam) || !ownsMenu(reinterpret_cast<HMENU>(wParam))) return false;
        break;
    case WM_MENUCHAR:
        if (!ownsMenu(reinterpret_cast<HMENU>(lParam))) return false;
        break;
    case WM_DRAWITEM: {
        if (wParam || !lParam) return false;
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw->CtlType != ODT_MENU || !ownsMenu(reinterpret_cast<HMENU>(draw->hwndItem)))
            return false;
        break;
    }
    case WM_MEASUREITEM:
        if (wParam || !lParam ||
            reinterpret_cast<const MEASUREITEMSTRUCT*>(lParam)->CtlType != ODT_MENU)
            return false;
        break;
    default:
        return false;
    }
    if (context3_) {
        LRESULT nativeResult = 0;
        if (context3_->HandleMenuMsg2(message, wParam, lParam, &nativeResult) == S_OK) {
            result = nativeResult;
            return true;
        }
    }
    // CM2 cannot return WM_MENUCHAR's MNC_* result; leave that to the owner.
    if (context2_ && message != WM_MENUCHAR &&
        context2_->HandleMenuMsg(message, wParam, lParam) == S_OK) {
        result = (message == WM_DRAWITEM || message == WM_MEASUREITEM) ? TRUE : 0;
        return true;
    }
    return false;
}

} // namespace explorer
