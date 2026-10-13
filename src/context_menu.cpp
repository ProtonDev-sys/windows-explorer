#include "explorer/context_menu.hpp"

#include <shlobj.h>
#include <array>
#include <algorithm>
#include <climits>
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

bool sameVerb(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() && left.size() <= INT_MAX &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                             static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

void findVerb(const std::vector<ContextMenuEntry>& entries, std::wstring_view verb,
              std::vector<UINT>& path, std::vector<UINT>& found, unsigned& matches,
              bool ancestorsEnabled, bool& enabled, size_t& matchedDepth) {
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        path.push_back(static_cast<UINT>(index));
        const bool usable = ancestorsEnabled && entry.enabled();
        if (!entry.separator() && sameVerb(entry.canonicalVerb, verb)) {
            if (path.size() < matchedDepth) {
                matches = 1; found = path; enabled = usable; matchedDepth = path.size();
            } else if (path.size() == matchedDepth) ++matches;
        }
        findVerb(entry.children, verb, path, found, matches, usable, enabled, matchedDepth);
        path.pop_back();
    }
}

unsigned commandOccurrences(const std::vector<ContextMenuEntry>& entries, UINT id) noexcept {
    unsigned count = 0;
    for (const auto& entry : entries) {
        if (!entry.separator() && entry.id == id) ++count;
        count += commandOccurrences(entry.children, id);
    }
    return count;
}

void findCommandPath(const std::vector<ContextMenuEntry>& entries, UINT id,
                     std::vector<UINT>& path, std::vector<UINT>& found, unsigned& matches,
                     bool ancestorsEnabled, bool& enabled) {
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        path.push_back(static_cast<UINT>(index));
        const bool usable = ancestorsEnabled && entry.enabled();
        if (!entry.separator() && entry.id == id) {
            ++matches;
            if (matches == 1) { found = path; enabled = usable; }
        }
        findCommandPath(entry.children, id, path, found, matches, usable, enabled);
        path.pop_back();
    }
}
} // namespace

NativeContextMenu::~NativeContextMenu() { reset(); }

void NativeContextMenu::reset() noexcept {
    ++generation_;
    plannedPopups_.clear();
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
    return createImpl<false>(owner, context, site, flags, false);
}

HRESULT NativeContextMenu::createLeafState(IContextMenu* context, IUnknown* site, UINT flags,
                                         bool omitResourceVerbs) {
    return createImpl<false>(nullptr, context, site, flags, true, omitResourceVerbs);
}

HRESULT NativeContextMenu::createForPlanning(HWND owner, IContextMenu* context,
                                            IUnknown* site, UINT flags) {
    return createImpl<false>(owner, context, site, flags, false, false, false);
}

HRESULT NativeContextMenu::create(HWND owner, IContextMenu* context, IUnknown* site,
                                  UINT flags, NativeContextMenuCreateObserver& observer) {
    observer.receipt = {};
    const HRESULT hr = createImpl<true>(owner, context, site, flags, false, false, true, &observer);
    observer.receipt.creationResult = hr;
    return hr;
}

HRESULT NativeContextMenu::createLeafState(IContextMenu* context, IUnknown* site, UINT flags,
                                         bool omitResourceVerbs, NativeContextMenuCreateObserver& observer) {
    observer.receipt = {};
    const HRESULT hr = createImpl<true>(nullptr, context, site, flags, true, omitResourceVerbs, true, &observer);
    observer.receipt.creationResult = hr;
    return hr;
}

template<bool Observed>
HRESULT NativeContextMenu::createImpl(HWND owner, IContextMenu* context, IUnknown* site,
                                     UINT flags, bool leafStateOnly, bool omitResourceVerbs,
                                     bool synchronousCascades, [[maybe_unused]] NativeContextMenuCreateObserver* observer) {
    // Retain inputs first: a caller may rebuild using this object's current menu.
    ComPtr<IContextMenu> retained = context;
    ComPtr<IUnknown> retainedSite = site;
    reset();
    const auto generation = generation_;
    if (!retained) return E_INVALIDARG;
    owner_ = owner;
    thread_ = GetCurrentThreadId();
    leafStateOnly_ = leafStateOnly;
    context_ = retained;
    [[maybe_unused]] IContextMenu* const originalProvider = retained.Get();
    [[maybe_unused]] const auto current = [&] { return generation == generation_ && context_.Get() == originalProvider; };
    // The unobserved specialization has no diagnostic invocation or retention.
    // Before/after diagnostics may pump. Freeze each native result before the
    // callback, then check the original generation/provider again afterwards.
    [[maybe_unused]] const auto boundary = [&]([[maybe_unused]] const char* operation, [[maybe_unused]] bool after,
        [[maybe_unused]] HRESULT hr, [[maybe_unused]] bool attempted) {
        if constexpr (!Observed) return S_OK;
        else {
            auto& receipt = observer->receipt;
            receipt.last = {operation, after, attempted, hr, generation, current(), receipt.last.sequence + 1};
            if (after && attempted && FAILED(hr)) receipt.lastNativeFailure = receipt.last;
            HRESULT diagnostic = S_OK;
            bool threw = false;
            try {
                if (observer->record) observer->record(observer->state, receipt.last);
            } catch (const std::bad_alloc&) { diagnostic = E_OUTOFMEMORY; threw = true; }
              catch (...) { diagnostic = E_FAIL; threw = true; }
            receipt.sourceCurrent = current();
            receipt.last.sourceCurrent = receipt.sourceCurrent;
            if (SUCCEEDED(diagnostic) && !receipt.sourceCurrent) diagnostic = E_ABORT;
            if (FAILED(diagnostic) && SUCCEEDED(receipt.diagnosticResult)) {
                receipt.firstDiagnosticFailure = receipt.last;
                receipt.diagnosticResult = diagnostic;
            }
            receipt.observerThrew = receipt.observerThrew || threw;
            return diagnostic;
        }
    };
    struct CallResult { HRESULT native; HRESULT diagnostic; bool attempted; };
    const auto call = [&]([[maybe_unused]] const char* operation, const auto& nativeCall,
        [[maybe_unused]] bool restore = false) {
        if constexpr (!Observed) return CallResult{nativeCall(), S_OK, true};
        else {
            const HRESULT before = boundary(operation, false, E_PENDING, false);
            // Restoring this same retained provider is required even if a
            // diagnostic throws or invalidates the wrapper. Never skip cleanup.
            if (FAILED(before) && !restore) return CallResult{E_PENDING, before, false};
            const HRESULT hr = nativeCall();
            const HRESULT after = boundary(operation, true, hr, true);
            return CallResult{hr, FAILED(before) ? before : after, true};
        }
    };
    const auto fail = [&](HRESULT hr) {
        // A pumped callback may have rebuilt this instance. Never reset the new
        // generation as cleanup for this abandoned creation.
        if constexpr (Observed) {
            if (current()) reset();
            observer->receipt.sourceCurrent = current();
        }
        else reset();
        return hr;
    };
    [[maybe_unused]] ComPtr<IContextMenu2> queried2;
    [[maybe_unused]] const auto query2 = call("QI.IContextMenu2", [&] {
        if constexpr (Observed) {
            const HRESULT hr = retained.As(&queried2);
            if (current()) context2_.Swap(queried2);
            return hr;
        } else return context_.As(&context2_);
    });
    if constexpr (Observed) { if (FAILED(query2.diagnostic)) return fail(query2.diagnostic); }
    [[maybe_unused]] ComPtr<IContextMenu3> queried3;
    [[maybe_unused]] const auto query3 = call("QI.IContextMenu3", [&] {
        if constexpr (Observed) {
            const HRESULT hr = retained.As(&queried3);
            if (current()) context3_.Swap(queried3);
            return hr;
        } else return context_.As(&context3_);
    });
    if constexpr (Observed) { if (FAILED(query3.diagnostic)) return fail(query3.diagnostic); }
    [[maybe_unused]] ComPtr<IObjectWithSite> queriedSite;
    [[maybe_unused]] const auto querySite = call("QI.IObjectWithSite", [&] {
        if constexpr (Observed) {
            const HRESULT hr = retained.As(&queriedSite);
            if (current()) objectWithSite_.Swap(queriedSite);
            return hr;
        } else return context_.As(&objectWithSite_);
    });
    if constexpr (Observed) { if (FAILED(querySite.diagnostic)) return fail(querySite.diagnostic); }
    if (generation != generation_) return E_ABORT;
    if (retainedSite && objectWithSite_) {
        [[maybe_unused]] ComPtr<IObjectWithSite> siteTarget;
        if constexpr (Observed) siteTarget = objectWithSite_;
        const auto attached = call("SetSite.attach", [&] {
            if constexpr (Observed) {
                const HRESULT hr = siteTarget->SetSite(retainedSite.Get());
                // Publish successful attachment before an after observer can
                // reset the wrapper, so reset detaches the original site.
                if (SUCCEEDED(hr) && current()) siteAttached_ = true;
                return hr;
            } else return objectWithSite_->SetSite(retainedSite.Get());
        });
        const HRESULT hr = attached.native;
        if (generation != generation_) return E_ABORT;
        if (FAILED(hr) && attached.attempted) return fail(hr);
        if constexpr (Observed) { if (FAILED(attached.diagnostic)) return fail(attached.diagnostic); }
        siteAttached_ = true;
    }
    ComPtr<IDefaultFolderMenuInitialize> configuration;
    DEFAULT_FOLDER_MENU_RESTRICTIONS previous = DFMR_DEFAULT;
    bool restrictedResources = false;
    const auto restoreRestrictions = [&] {
        return call("SetMenuRestrictions.restore", [&] { return configuration->SetMenuRestrictions(previous); }, true);
    };
    [[maybe_unused]] const auto failRestricted = [&](HRESULT hr) {
        if (restrictedResources) restoreRestrictions();
        return fail(hr);
    };
    if (leafStateOnly && omitResourceVerbs) {
        const auto queried = call("QI.IDefaultFolderMenuInitialize", [&] {
            if constexpr (Observed) return retained.As(&configuration);
            else return context_.As(&configuration);
        });
        if constexpr (Observed) {
            if (FAILED(queried.diagnostic)) return fail(queried.attempted && FAILED(queried.native) && queried.native != E_NOINTERFACE ? queried.native : queried.diagnostic);
        }
        HRESULT restriction = queried.native;
        if (restriction != E_NOINTERFACE) {
            // Preserve every documented restriction. Only unrelated built-in
            // operation entries are omitted; association and dynamic native
            // handlers still receive the original selection and view site.
            constexpr auto mask = static_cast<DEFAULT_FOLDER_MENU_RESTRICTIONS>(
                DFMR_NO_STATIC_VERBS | DFMR_STATIC_VERBS_ONLY | DFMR_NO_RESOURCE_VERBS |
                DFMR_OPTIN_HANDLERS_ONLY | DFMR_RESOURCE_AND_FOLDER_VERBS_ONLY |
                DFMR_USE_SPECIFIED_HANDLERS | DFMR_USE_SPECIFIED_VERBS | DFMR_NO_ASYNC_VERBS |
                DFMR_NO_NATIVECPU_VERBS | DFMR_NO_NONWOW_VERBS);
            if (SUCCEEDED(restriction)) {
                const auto read = call("GetMenuRestrictions", [&] { return configuration->GetMenuRestrictions(mask,&previous); });
                restriction = read.native;
                if constexpr (Observed) {
                    if (FAILED(read.diagnostic)) return fail(read.attempted && FAILED(restriction) ? restriction : read.diagnostic);
                }
            }
            if (SUCCEEDED(restriction) && !(previous & DFMR_NO_RESOURCE_VERBS)) {
                const auto changed = call("SetMenuRestrictions.omitResource", [&] {
                    return configuration->SetMenuRestrictions(
                        static_cast<DEFAULT_FOLDER_MENU_RESTRICTIONS>(previous | DFMR_NO_RESOURCE_VERBS));
                });
                restriction = changed.native;
                restrictedResources = changed.attempted && SUCCEEDED(restriction);
                if constexpr (Observed) {
                    if (FAILED(changed.diagnostic)) return failRestricted(changed.attempted && FAILED(restriction) ? restriction : changed.diagnostic);
                }
            }
            if (FAILED(restriction)) return fail(restriction);
        }
    }
    const auto popupCreation = call("CreatePopupMenu", [&] {
        menu_ = CreatePopupMenu();
        return menu_ ? S_OK : menuError();
    });
    if constexpr (Observed) {
        if (FAILED(popupCreation.diagnostic)) return failRestricted(popupCreation.attempted && FAILED(popupCreation.native) ? popupCreation.native : popupCreation.diagnostic);
    }
    if (!menu_) {
        const HRESULT hr = popupCreation.native;
        if (restrictedResources) restoreRestrictions();
        return fail(hr);
    }
    popup_ = menu_;
    // Ordinary popups retain synchronous cascades. Exact planning and the
    // read-only leaf worker inspect native metadata before any branch request.
    const UINT nativeFlags = leafStateOnly || !synchronousCascades
        ? flags & ~CMF_SYNCCASCADEMENU : flags | CMF_SYNCCASCADEMENU;
    const HMENU originalMenu = menu_;
    const auto queried = call("QueryContextMenu", [&] {
        return retained->QueryContextMenu(originalMenu, 0, firstCommand_, lastCommand_,
                                          nativeFlags);
    });
    const HRESULT hr = queried.native;
    // A custom namespace can retain this same provider for a later normal
    // popup. Restrict only this query; canonical readback below must still use
    // the native menu that was just created, with the provider's original flags.
    const auto restoration = restrictedResources ? restoreRestrictions() : CallResult{S_OK, S_OK, false};
    const HRESULT restored = restoration.native;
    if (generation != generation_) return E_ABORT;
    if (FAILED(hr) && queried.attempted) return fail(hr);
    if (FAILED(restored)) return fail(restored);
    if constexpr (Observed) {
        if (FAILED(queried.diagnostic)) return fail(queried.diagnostic);
        if (FAILED(restoration.diagnostic)) return fail(restoration.diagnostic);
    }
    if constexpr (Observed) {
        const UINT commands = HRESULT_CODE(hr);
        if (commands > lastCommand_ - firstCommand_ + 1) return fail(E_UNEXPECTED);
        // Release the same original temporaries in their ordinary destruction
        // order before accepting success. Release itself can pump. The raw
        // original-provider address is only compared, never dereferenced here.
        configuration.Reset();
        queriedSite.Reset();
        queried3.Reset();
        queried2.Reset();
        retainedSite.Reset();
        retained.Reset();
        observer->receipt.sourceCurrent = current();
        if (!observer->receipt.sourceCurrent) {
            if (SUCCEEDED(observer->receipt.diagnosticResult)) {
                observer->receipt.diagnosticResult = E_ABORT;
                observer->receipt.firstDiagnosticFailure = {
                    "completionFence", false, false, E_PENDING, generation, false, observer->receipt.last.sequence + 1};
            }
            return E_ABORT;
        }
        commandCount_ = commands;
    } else {
        commandCount_ = HRESULT_CODE(hr);
        if (commandCount_ > lastCommand_ - firstCommand_ + 1) return fail(E_UNEXPECTED);
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

HRESULT NativeContextMenu::createSelectionForPlanning(HWND owner, IShellItemArray* selection,
                                                      IUnknown* site, UINT flags) {
    reset();
    if (!selection) return E_INVALIDARG;
    DWORD count = 0;
    HRESULT hr = selection->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count) return E_INVALIDARG;
    ComPtr<IContextMenu> context;
    hr = selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&context));
    return FAILED(hr) ? hr : createForPlanning(owner, context.Get(), site, flags | CMF_ITEMMENU);
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
    const auto retained = context_;
    if (!retained) return {};
    std::array<wchar_t, 512> wide{};
    HRESULT hr = retained->GetCommandString(id - firstCommand_, GCS_VERBW, nullptr,
                                            reinterpret_cast<LPSTR>(wide.data()),
                                            static_cast<UINT>(wide.size()));
    wide.back() = L'\0';
    if (SUCCEEDED(hr)) return wide.data();
    std::array<char, 512> ansi{};
    hr = retained->GetCommandString(id - firstCommand_, GCS_VERBA, nullptr, ansi.data(),
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
                                         bool populate, bool strictMessages) {
    if (depth > maximumDepth || !budget) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const auto generation = generation_;
    if (populate) {
        if (strictMessages) {
            const HRESULT hr = initializeForPlanning(menu, position);
            if (FAILED(hr)) return hr;
        } else {
            LRESULT ignored = 0;
            handleMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu),MAKELPARAM(position, FALSE), ignored);
        }
    }
    if (generation != generation_) return E_ABORT;
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
        if (generation != generation_) return E_ABORT;
        if (info.hSubMenu) {
            const HRESULT hr = enumerateMenu(info.hSubMenu, static_cast<UINT>(i), depth + 1,
                                             budget, entry.children,
                                             populate && (!strictMessages || entry.enabled()), strictMessages);
            if (FAILED(hr)) return hr;
        }
        if (generation != generation_) return E_ABORT;
        entries.push_back(std::move(entry));
    }
    return S_OK;
}

HRESULT NativeContextMenu::initializeForPlanning(HMENU menu, UINT position) {
    if (!context_ || !menu_ || !ownsMenu(menu)) return E_INVALIDARG;
    if (thread_ != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (leafStateOnly_) return E_ACCESSDENIED;
    const auto prior = std::find_if(plannedPopups_.begin(), plannedPopups_.end(),
                                   [menu](const PlannedPopup& popup) { return popup.menu == menu; });
    if (prior != plannedPopups_.end()) return prior->status;
    // Publish the pending attempt before the native callback. Reentrant exact
    // planning cannot initialize this same popup a second time.
    plannedPopups_.push_back({menu, E_PENDING});
    const auto generation = generation_;
    const auto retained = context_;
    const auto retained3 = context3_;
    const auto retained2 = context2_;
    HRESULT hr = S_OK; // A static IContextMenu has no delayed-message contract.
    if (retained3) {
        LRESULT ignored = 0;
        hr = retained3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu),
                                     MAKELPARAM(position, FALSE), &ignored);
        if (generation != generation_) return E_ABORT;
    } else if (retained2) {
        hr = retained2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu),
                                     MAKELPARAM(position, FALSE));
    }
    if (generation != generation_ || context_.Get() != retained.Get() || !ownsMenu(menu)) return E_ABORT;
    if (SUCCEEDED(hr) && hr != S_OK) hr = E_UNEXPECTED;
    const auto completed = std::find_if(plannedPopups_.begin(), plannedPopups_.end(),
                                       [menu](const PlannedPopup& popup) { return popup.menu == menu; });
    if (completed == plannedPopups_.end()) return E_ABORT;
    completed->status = hr;
    return hr;
}

HRESULT NativeContextMenu::enumerateForVerbs(std::span<const std::wstring_view> verbs,
                                            std::vector<ContextMenuEntry>& entries) {
    return enumerateForTarget(verbs, 0, entries);
}

HRESULT NativeContextMenu::enumerateForCommand(UINT actualNativeId, std::vector<ContextMenuEntry>& entries) {
    if (actualNativeId < firstCommand_ || actualNativeId > lastCommand_) { entries.clear(); return E_INVALIDARG; }
    return enumerateForTarget({}, actualNativeId, entries);
}

HRESULT NativeContextMenu::enumerateForTarget(std::span<const std::wstring_view> verbs, UINT actualNativeId,
                                             std::vector<ContextMenuEntry>& entries) {
    entries.clear();
    if (!context_ || !menu_) return E_UNEXPECTED;
    if (thread_ != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (verbs.size() > maximumEntries) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    for (const auto verb : verbs)
        if (verb.empty() || verb.size() >= 512 || verb.find(L'\0') != std::wstring_view::npos) return E_INVALIDARG;
    try {
        const auto generation = generation_;
        const auto retained = context_;
        std::vector<ContextMenuEntry> snapshot;
        unsigned budget = maximumEntries;
        HRESULT hr = enumerateMenu(popup_, 0, 0, budget, snapshot, false);
        if (FAILED(hr)) return hr;
        const size_t requests = actualNativeId ? 1 : verbs.size();
        for (size_t request = 0; request < requests; ++request) {
            std::vector<UINT> path, found;
            unsigned matches = 0;
            bool enabled = false;
            if (actualNativeId) findCommandPath(snapshot, actualNativeId, path, found, matches, true, enabled);
            else {
                size_t matchedDepth = maximumDepth + 2;
                findVerb(snapshot, verbs[request], path, found, matches, true, enabled, matchedDepth);
            }
            if (matches > 1) return E_UNEXPECTED;
            if (!matches) {
                if (actualNativeId) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                continue;
            }
            HMENU branch = popup_;
            const std::vector<ContextMenuEntry>* level = &snapshot;
            unsigned depth = 0;
            UINT position = 0;
            const ContextMenuEntry* matched = nullptr;
            for (const auto index : found) {
                if (index >= level->size()) return E_UNEXPECTED;
                matched = &(*level)[index];
                MENUITEMINFOW native{sizeof(native)};
                native.fMask = MIIM_ID | MIIM_SUBMENU;
                if (!GetMenuItemInfoW(branch, index, TRUE, &native)) return menuError();
                if (native.wID != matched->id || (native.hSubMenu != nullptr) != matched->submenu) return E_ABORT;
                position = index;
                branch = native.hSubMenu;
                level = &matched->children;
                ++depth;
            }
            if (!matched || matched->id < firstCommand_ || matched->id - firstCommand_ >= commandCount_)
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (commandOccurrences(snapshot, matched->id) != 1) return E_UNEXPECTED;
            if (matched && matched->submenu && enabled) {
                if (leafStateOnly_) return E_ACCESSDENIED;
                std::vector<ContextMenuEntry> populated;
                // Account for entries outside this branch when applying the
                // same complete-tree bound used by ordinary enumeration.
                unsigned oldBranchCount = maximumEntries;
                std::vector<ContextMenuEntry> oldBranch;
                hr = enumerateMenu(branch, position, depth, oldBranchCount, oldBranch, false);
                if (FAILED(hr)) return hr;
                budget += maximumEntries - oldBranchCount;
                hr = enumerateMenu(branch, position, depth, budget, populated, true, true);
                if (FAILED(hr)) return hr;
                snapshot.clear(); budget = maximumEntries;
                hr = enumerateMenu(popup_, 0, 0, budget, snapshot, false);
                if (FAILED(hr)) return hr;
            }
            break; // Original ordered-alias authority, including disabled rows.
        }
        if (generation != generation_ || context_.Get() != retained.Get()) return E_ABORT;
        entries.swap(snapshot);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
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
    // InvokeCommand may pump the owner STA. Retain the handler across any
    // callback that refreshes the host's menu collections.
    const auto retained = context_;
    return retained->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
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
