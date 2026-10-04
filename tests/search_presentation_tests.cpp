#include "explorer/search.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/search_presentation_store.hpp"
#include "explorer/headless_visual.hpp"
#include <shlobj.h>
#include <propkey.h>
#include <wrl/implements.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::cerr << "HRESULT=0x" << std::hex << static_cast<ULONG>(hr) << std::dec << '\n'; throw std::runtime_error(message); }
}
void pump() { MSG m{}; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }
void waitFor(const std::function<bool()>& ready, const char* message) {
    const auto deadline = GetTickCount64() + 10000;
    while (!ready()) { require(GetTickCount64() < deadline, message); MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE); pump(); }
}
struct FileStamp {
    ULONGLONG volume{};
    std::array<BYTE, 16> id{};
    LONGLONG size{}, modified{}, changed{};
    DWORD attributes{};
    std::string bytes;
    bool operator==(const FileStamp&) const = default;
};
FileStamp stamp(const fs::path& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Open exact owned fixture for preservation readback");
    FILE_ID_INFO identity{}; FILE_STANDARD_INFO standard{}; FILE_BASIC_INFO basic{};
    const bool valid = GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) &&
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(handle); require(valid, "Read owned fixture identity and timestamps");
    std::ifstream stream(path, std::ios::binary); require(stream.good(), "Read owned content");
    FileStamp value; value.volume = identity.VolumeSerialNumber;
    std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), value.id.begin());
    value.size = standard.EndOfFile.QuadPart; value.modified = basic.LastWriteTime.QuadPart; value.changed = basic.ChangeTime.QuadPart;
    value.attributes = basic.FileAttributes; value.bytes.assign(std::istreambuf_iterator<char>(stream), {});
    return value;
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream << bytes; require(stream.good(), "Write owned presentation fixture");
}
ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> value; succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&value)), "Create owned Shell identity"); return value;
}
struct Fixture {
    fs::path root, scope;
    Fixture() {
        GUID id{}; succeeded(CoCreateGuid(&id), "Create unique owned presentation root"); wchar_t text[40]{};
        require(StringFromGUID2(id, text, 40) != 0, "Format owned root");
        root = fs::temp_directory_path() / (std::wstring(L"WindowsExplorer-OwnedSearchPresentation-") + text);
        require(fs::create_directory(root), "Create unique fixture without overwrite");
        scope = root / L"scope"; require(fs::create_directory(scope), "Create isolated search scope");
        write(scope / L"member-a.txt", "owned A"); write(scope / L"member-b.bin", "owned B with different size");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
class Events final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerBrowserEvents> {
public:
    bool done = false; HRESULT result = E_PENDING;
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { done = false; result = E_PENDING; return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override { done = true; result = S_OK; return S_OK; }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override { done = true; result = E_FAIL; return S_OK; }
};
class Site final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IServiceProvider, IExplorerPaneVisibility> {
public:
    IFACEMETHODIMP QueryService(REFGUID service, REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        return service == SID_ExplorerPaneVisibility ? QueryInterface(iid, output) : E_NOINTERFACE;
    }
    IFACEMETHODIMP GetPaneState(REFEXPLORERPANE, EXPLORERPANESTATE* state) override {
        if (!state) return E_POINTER; *state = static_cast<EXPLORERPANESTATE>(EPS_DEFAULT_OFF | EPS_FORCE); return S_OK;
    }
};
class Browser final {
public:
    ~Browser() {
        if (view_) { ComPtr<IObjectWithSite> object; if (SUCCEEDED(view_.As(&object))) object->SetSite(nullptr); }
        folder_.Reset(); view_.Reset();
        if (browser_) { browser_->Unadvise(cookie_); ComPtr<IObjectWithSite> object; if (SUCCEEDED(browser_.As(&object))) object->SetSite(nullptr); browser_->Destroy(); browser_.Reset(); }
        if (window_) DestroyWindow(window_); pump();
    }
    void open(IShellItem* target, IShellItem* allowedScope) {
        allowedScope_ = allowedScope;
        require(explorer::PrivateDesktop::current() != nullptr, "Only create native views on an owned private desktop");
        window_ = CreateWindowExW(0, L"STATIC", L"Owned saved-search presentation fixture", WS_OVERLAPPEDWINDOW,
                                 0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window_ && !IsWindowVisible(window_), "Native fixture host must be hidden");
        succeeded(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser_)), "Create fresh native browser");
        site_ = Make<Site>(); ComPtr<IObjectWithSite> object; succeeded(browser_.As(&object), "Read public browser site");
        succeeded(object->SetSite(static_cast<IServiceProvider*>(site_.Get())), "Attach isolated pane site");
        succeeded(browser_->SetOptions(static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_NOPERSISTVIEWSTATE | EBO_NOTRAVELLOG | EBO_NOBORDER)), "Disable shared native view persistence");
        FOLDERSETTINGS settings{static_cast<UINT>(FVM_AUTO), FWF_AUTOARRANGE}; RECT bounds{0, 0, 1000, 700};
        succeeded(browser_->Initialize(window_, &bounds, &settings), "Initialize hidden native browser");
        events_ = Make<Events>(); succeeded(browser_->Advise(events_.Get(), &cookie_), "Observe native navigation");
        succeeded(browser_->BrowseToObject(target, SBSP_ABSOLUTE), "Open owned saved search through its native handler");
        waitFor([&] { return events_->done; }, "Native presentation fixture navigation timed out"); succeeded(events_->result, "Native saved search navigation");
        succeeded(browser_->GetCurrentView(IID_PPV_ARGS(&view_)), "Read actual native view"); succeeded(view_.As(&folder_), "Read actual IFolderView2");
        waitFor([&] { int count = 0; return SUCCEEDED(folder_->ItemCount(SVGIO_ALLVIEW, &count)) && count == 2; }, "Native results must enumerate both owned members");
        HWND child = nullptr; succeeded(view_->GetWindow(&child), "Read native view ownership"); DWORD process = 0;
        require(GetWindowThreadProcessId(child, &process) == GetCurrentThreadId() && process == GetCurrentProcessId() &&
                IsChild(window_, child) && !IsWindowVisible(child), "Native view remains an invisible owned child");
    }
    IFolderView2* view() const { return folder_.Get(); }
    void verifyMembers(const std::set<std::array<BYTE, 16>>& expected) const {
        // Native view-mode changes can recreate their item collection. Wait
        // for real identities rather than trusting the earlier item count.
        waitFor([&] {
            std::set<std::array<BYTE, 16>> actual;
            int count = 0; if (FAILED(folder_->ItemCount(SVGIO_ALLVIEW, &count)) || count != 2) return false;
            for (int index = 0; index < count; ++index) {
                ComPtr<IShellItem> member; if (FAILED(folder_->GetItem(index, IID_PPV_ARGS(&member)))) return false;
                PWSTR raw = nullptr; const auto hr = member->GetDisplayName(SIGDN_FILESYSPATH, &raw);
                if (FAILED(hr) || !raw) { CoTaskMemFree(raw); return false; }
                const fs::path path(raw); CoTaskMemFree(raw);
                auto parent = item(path.parent_path());
                int comparison = 1; succeeded(parent->Compare(allowedScope_.Get(), SICHINT_CANONICAL, &comparison), "Check filesystem parent ownership before reading result bytes");
                require(comparison == 0, "Native result escaped the exact owned scope");
                actual.insert(stamp(path).id);
            }
            return actual == expected;
        }, "Native saved search result identities changed during presentation round trip");
    }
private:
    HWND window_ = nullptr; DWORD cookie_ = 0;
    ComPtr<IExplorerBrowser> browser_; ComPtr<IShellView> view_; ComPtr<IFolderView2> folder_; ComPtr<Events> events_; ComPtr<Site> site_;
    ComPtr<IShellItem> allowedScope_;
};
bool same(const explorer::SearchViewPresentation& a, const explorer::SearchViewPresentation& b) {
    if (a.mode != b.mode || a.iconSize != b.iconSize || a.visibleColumns != b.visibleColumns || a.groupBy.has_value() != b.groupBy.has_value() || a.sort.has_value() != b.sort.has_value()) return false;
    const auto order = [](const explorer::SearchViewOrder& x, const explorer::SearchViewOrder& y) { return x.property == y.property && x.direction == y.direction; };
    if (a.groupBy && !order(*a.groupBy, *b.groupBy)) return false;
    if (a.sort) { if (a.sort->size() != b.sort->size()) return false; for (size_t i = 0; i < a.sort->size(); ++i) if (!order((*a.sort)[i], (*b.sort)[i])) return false; }
    return true;
}
void presentationRoundTrips() {
    Fixture fixture; const auto sourceA = stamp(fixture.scope / L"member-a.txt"), sourceB = stamp(fixture.scope / L"member-b.bin");
    const std::set<std::array<BYTE, 16>> identities{sourceA.id, sourceB.id}; auto scope = item(fixture.scope);
    unsigned number = 0;
    const std::array<std::pair<explorer::SearchViewMode, int>, 8> layouts{{
        {explorer::SearchViewMode::Icons, 256}, {explorer::SearchViewMode::Icons, 96}, {explorer::SearchViewMode::Icons, 48},
        {explorer::SearchViewMode::SmallIcons, 16}, {explorer::SearchViewMode::List, 16}, {explorer::SearchViewMode::Details, 16},
        {explorer::SearchViewMode::Tiles, 48}, {explorer::SearchViewMode::Content, 32}}};
    const auto companionDirectory = fixture.root / L"app-owned-view-records";
    for (const auto& [mode, size] : layouts) {
        for (const bool grouped : {false, true}) {
            explorer::SearchViewPresentation expected; expected.mode = mode;
            expected.iconSize = size;
            expected.visibleColumns = std::vector<std::wstring>{L"System.ItemNameDisplay", L"System.Size", L"System.DateModified", L"System.ItemTypeText"};
            expected.groupBy = explorer::SearchViewOrder{grouped ? L"System.ItemTypeText" : L"System.Null", grouped ? SORT_DESCENDING : SORT_ASCENDING};
            expected.sort = std::vector<explorer::SearchViewOrder>{{L"System.Size", SORT_DESCENDING}, {L"System.ItemNameDisplay", SORT_ASCENDING}};
            const auto path = fixture.root / (L"saved-" + std::to_wstring(++number) + L".search-ms");
            explorer::SearchViewPresentation native;
            succeeded(explorer::nativeSearchViewPresentation(expected, &native), "Project the exact supported public XML fields");
            succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, path, explorer::SearchSaveMode::CreateNew, &native), "Save typed public presentation");
            succeeded(explorer::saveSearchPresentationCompanion(path, companionDirectory, expected), "Save app-owned exact native layout companion");
            const auto savedBefore = stamp(path); explorer::SavedSearchMetadata imported;
            succeeded(explorer::readSavedSearch(path, &imported), "Import supported presentation"); require(imported.presentation && same(*imported.presentation, native), "Imported presentation changed public fields");
            require(explorer::loadSearchPresentationCompanion(path, companionDirectory, &*imported.presentation) == S_OK, "Read exact matching owned companion");
            require(same(*imported.presentation, expected), "Companion did not preserve all actual native layout fields");
            {
                Browser browser; browser.open(item(path).Get(), scope.Get());
                succeeded(explorer::applySearchViewPresentation(browser.view(), *imported.presentation), "Apply imported public metadata to actual native view");
                explorer::SearchViewPresentation captured;
                succeeded(explorer::captureSearchViewPresentation(browser.view(), &captured), "Capture actual native presentation");
                require(same(captured, expected), "Actual native mode/size/columns/group/sort differs from saved presentation"); browser.verifyMembers(identities);
                const auto edited = fixture.root / (L"edited-" + std::to_wstring(number) + L".search-ms");
                succeeded(explorer::nativeSearchViewPresentation(captured, &native), "Project refined native presentation");
                succeeded(explorer::saveSearchForScopeRules(imported.query + L" AND System.Size:>=1", imported.scopeRules, edited, explorer::SearchSaveMode::CreateNew, &native), "Refine and re-save exact native view metadata");
                succeeded(explorer::saveSearchPresentationCompanion(edited, companionDirectory, captured), "Persist refined exact native layout");
                explorer::SavedSearchMetadata restored; succeeded(explorer::readSavedSearch(edited, &restored), "Re-import refined search");
                require(restored.presentation && explorer::loadSearchPresentationCompanion(edited, companionDirectory, &*restored.presentation) == S_OK && same(*restored.presentation, expected), "Refinement discarded presentation metadata");
                Browser fresh; fresh.open(item(edited).Get(), scope.Get()); succeeded(explorer::applySearchViewPresentation(fresh.view(), *restored.presentation), "Apply metadata after fresh native browser recreation");
                explorer::SearchViewPresentation final; succeeded(explorer::captureSearchViewPresentation(fresh.view(), &final), "Read fresh native presentation");
                require(same(final, expected), "Destroy/recreate or re-save changed native presentation"); fresh.verifyMembers(identities);
            }
            require(stamp(path) == savedBefore, "Native open/import/refinement changed original saved file");
        }
    }
    require(stamp(fixture.scope / L"member-a.txt") == sourceA && stamp(fixture.scope / L"member-b.bin") == sourceB, "Presentation changes mutated owned contents, identity, attributes or timestamps");
}
void malformedAndFailurePreservation() {
    Fixture fixture; auto scope = item(fixture.scope); const auto output = fixture.root / L"protected.search-ms";
    succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, output), "Create valid presentation sentinel"); const auto before = stamp(output);
    explorer::SearchViewPresentation invalid; invalid.iconSize = 257;
    require(FAILED(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, output, explorer::SearchSaveMode::UserConfirmed, &invalid)) && stamp(output) == before,
            "Rejected presentation overwrote an existing saved search");
    const auto xml = before.bytes; unsigned index = 0;
    for (const auto& replacement : {
        std::string("<viewInfo viewMode=\"content\">"), std::string("<viewInfo iconSize=\"15\">"), std::string("<viewInfo folderFlags=\"1\">"),
        std::string("<viewInfo><stackList/>"), std::string("<viewInfo><visibleColumns><column viewField=\"Unknown.Unregistered.Property\"/></visibleColumns>"),
        std::string("<viewInfo><visibleColumns><column viewField=\"System.Size\"/><column viewField=\"System.Size\"/></visibleColumns>"),
        std::string("<viewInfo><groupBy viewField=\"System.Size\" direction=\"sideways\"/>"), std::string("<viewInfo><sortList><sort viewField=\"System.Size\" direction=\"ascending\"/><sort viewField=\"System.Size\" direction=\"descending\"/></sortList>"),
        std::string("<viewInfo><visibleColumns/>"), std::string("<viewInfo iconSize=\"256\" iconSize=\"16\">")}) {
        auto bytes = xml; const auto start = bytes.find("<viewInfo"), end = bytes.find('>', start);
        require(start != std::string::npos && end != std::string::npos, "Valid viewInfo test marker"); bytes.replace(start, end + 1 - start, replacement);
        const auto malformed = fixture.root / (L"unsupported-" + std::to_wstring(++index) + L".search-ms"); write(malformed, bytes);
        explorer::SavedSearchMetadata sentinel; sentinel.query = L"unchanged"; sentinel.presentation.emplace(); sentinel.presentation->iconSize = 72;
        require(FAILED(explorer::readSavedSearch(malformed, &sentinel)) && sentinel.query == L"unchanged" && sentinel.presentation && sentinel.presentation->iconSize == 72,
                "Unsupported presentation was discarded or changed caller output");
    }
    explorer::SearchViewPresentation sentinel; sentinel.iconSize = 72;
    require(explorer::captureSearchViewPresentation(nullptr, &sentinel) == E_POINTER && sentinel.iconSize == 72, "Failed capture changed caller state");
    require(explorer::applySearchViewPresentation(nullptr, {}) == E_POINTER, "Null native view accepted");
    require(stamp(output) == before, "Metadata rejection mutated the original valid saved search");
}
void companionIntegrityAndFailures() {
    Fixture fixture; auto scope = item(fixture.scope);
    const auto query = fixture.root / L"owned.search-ms", directory = fixture.root / L"own-cache";
    succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, query), "Create native-openable companion query");
    const auto queryBefore = stamp(query);
    explorer::SearchViewPresentation expected; expected.mode = explorer::SearchViewMode::Content; expected.iconSize = 32;
    succeeded(explorer::saveSearchPresentationCompanion(query, directory, expected), "Create owned companion");
    std::vector<fs::path> entries; for (const auto& entry : fs::directory_iterator(directory)) entries.push_back(entry.path());
    require(entries.size() == 1 && entries[0].extension() == L".dat", "Companion save left a staging file or unrelated artifact");
    const auto companion = entries[0]; const auto companionBefore = stamp(companion);
    expected.mode = explorer::SearchViewMode::List; expected.iconSize = 16;
    const auto locked = CreateFileW(companion.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "Lock only owned companion against replacement");
    const auto failed = explorer::saveSearchPresentationCompanion(query, directory, expected); CloseHandle(locked);
    require(FAILED(failed) && stamp(companion) == companionBefore && stamp(query) == queryBefore,
            "Failed atomic companion replacement changed the existing query or presentation");
    require(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 1, "Failed companion replacement left a staging file");
    require(SetFileAttributesW(companion.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != FALSE, "Set only owned companion metadata");
    succeeded(explorer::saveSearchPresentationCompanion(query, directory, expected), "Atomically replace exact owned companion");
    require((GetFileAttributesW(companion.c_str()) & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)) ==
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED), "Atomic companion replacement discarded existing basic attributes");
    explorer::SearchViewPresentation restored; restored.visibleColumns = std::vector<std::wstring>{L"System.ItemNameDisplay", L"System.Size"};
    require(explorer::loadSearchPresentationCompanion(query, directory, &restored) == S_OK && restored.mode == expected.mode &&
            restored.iconSize == expected.iconSize && restored.visibleColumns->size() == 2, "Companion replaced public imported columns or lost native mode");
    const auto copied = fixture.root / L"copied.search-ms"; require(fs::copy_file(query, copied), "Copy only owned query to distinct FileID");
    explorer::SearchViewPresentation sentinel; sentinel.mode = explorer::SearchViewMode::Tiles; sentinel.iconSize = 72;
    require(explorer::loadSearchPresentationCompanion(copied, directory, &sentinel) == S_FALSE && sentinel.mode == explorer::SearchViewMode::Tiles && sentinel.iconSize == 72,
            "Distinct file identity inherited a stale layout");
    // Own-file external edits retain FileID but invalidate size/timestamps.
    write(query, queryBefore.bytes + "\n<!-- owned external modification -->\n");
    require(stamp(query).id == queryBefore.id, "External-edit fixture unexpectedly replaced the file identity");
    require(explorer::loadSearchPresentationCompanion(query, directory, &sentinel) == S_FALSE && sentinel.iconSize == 72,
            "External modified query inherited its old companion");
    // Malformed/foreign cache records are never treated as replaceable app data.
    require(SetFileAttributesW(companion.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "Normalize only owned companion before corrupt-record fixture write");
    write(companion, "foreign owned-test sentinel"); const auto foreign = stamp(companion);
    require(FAILED(explorer::loadSearchPresentationCompanion(query, directory, &sentinel)) && sentinel.iconSize == 72,
            "Malformed companion changed caller state");
    require(FAILED(explorer::saveSearchPresentationCompanion(query, directory, expected)) && stamp(companion) == foreign,
            "Save silently overwrote a foreign or corrupt record");
    require(FAILED(explorer::saveSearchPresentationCompanion(query, fixture.scope / L"member-a.txt", expected)) &&
            stamp(fixture.scope / L"member-a.txt").bytes == "owned A", "Invalid store directory overwrote an owned file");
    require(explorer::loadSearchPresentationCompanion(query, directory, nullptr) == E_POINTER, "Null companion output accepted");
    require(explorer::saveSearchPresentationCompanion(query, L"relative-cache", expected) == E_INVALIDARG, "Relative companion directory accepted");
}
} // namespace
int main() {
    explorer::PrivateDesktop desktop;
    auto hr = desktop.initialize(); if (FAILED(hr)) { std::cerr << "FAIL: private desktop unavailable\n"; return 1; }
    hr = OleInitialize(nullptr); if (FAILED(hr)) { std::cerr << "FAIL: native STA unavailable\n"; return 1; }
    const DWORD clipboard = GetClipboardSequenceNumber(); int failures = 0;
    for (const auto& [name, test] : std::array<std::pair<const char*, void(*)()>, 3>{{
        {"saved-search actual native presentation import/refine/re-save/recreate", &presentationRoundTrips},
        {"saved-search unsupported presentation and failure preservation", &malformedAndFailurePreservation},
        {"app-owned exact-file companion atomic replacement, stale/foreign/error preservation", &companionIntegrityAndFailures}}}) {
        try { test(); std::cout << "PASS: " << name << '\n'; } catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
    }
    bool unchanged = false, visible = true;
    if (FAILED(desktop.verifyIsolation(&unchanged)) || !unchanged || FAILED(desktop.visibleWindowsOnInputDesktop(visible)) || visible || GetClipboardSequenceNumber() != clipboard) {
        ++failures; std::cerr << "FAIL: native presentation fixture desktop/clipboard isolation\n";
    }
    OleUninitialize(); std::cout << 3 - std::min(failures, 3) << "/3 native saved-search presentation groups passed\n";
    return failures ? 1 : 0;
}
