#include "explorer/live_search.hpp"
#include "explorer/search.hpp"
#include "explorer/headless_visual.hpp"

#include <shlobj.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <compare>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using explorer::LiveSearchKind;
using explorer::LiveSearchPolicy;
using explorer::LiveSearchRequest;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "Parse owned fixture");
    return result;
}
struct Identity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> id{};
    auto operator<=>(const Identity&) const = default;
};
struct Stamp {
    Identity identity;
    LONGLONG creation = 0, write = 0, change = 0, size = 0;
    DWORD attributes = 0;
    std::string bytes;
    bool operator==(const Stamp&) const = default;
};
Stamp stamp(const fs::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Open owned fixture identity");
    FILE_ID_INFO id{}; FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{};
    const bool ok = GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard));
    CloseHandle(handle); require(ok && !(basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Read owned fixture metadata");
    Stamp result; result.identity.volume = id.VolumeSerialNumber;
    std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), result.identity.id.begin());
    result.creation = basic.CreationTime.QuadPart; result.write = basic.LastWriteTime.QuadPart;
    result.change = basic.ChangeTime.QuadPart; result.size = standard.EndOfFile.QuadPart;
    result.attributes = basic.FileAttributes;
    std::ifstream stream(path, std::ios::binary); require(stream.good(), "Read owned fixture bytes");
    result.bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return result;
}
struct Fixture {
    fs::path root;
    ComPtr<IShellItem> scope;
    std::map<fs::path, Stamp> sources;
    Fixture() {
        GUID guid{}; succeeded(CoCreateGuid(&guid), "Create owned live search identifier");
        wchar_t name[40]{}; require(StringFromGUID2(guid, name, 40) != 0, "Format owned live search identifier");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-live-search-") + name);
        require(fs::create_directory(root), "Create exclusive owned live search folder");
        for (const auto& [leaf, contents] : std::array<std::pair<const wchar_t*, const char*>, 3>{{
            {L"alpha.txt", "owned alpha unchanged"}, {L"beta.bin", "owned beta unchanged"},
            {L"\u5b50-gamma.txt", "owned Unicode gamma unchanged"}}}) {
            const auto path = root / leaf;
            { std::ofstream stream(path, std::ios::binary); stream << contents; require(stream.good(), "Write owned live search fixture"); }
            sources.emplace(path, stamp(path));
        }
        scope = item(root);
    }
    ~Fixture() { scope.Reset(); std::error_code ignored; fs::remove_all(root, ignored); }
    std::set<Identity> identities(std::initializer_list<const wchar_t*> leaves) const {
        std::set<Identity> result;
        for (auto leaf : leaves) result.insert(sources.at(root / leaf).identity);
        return result;
    }
    void unchanged() const {
        require(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == static_cast<ptrdiff_t>(sources.size()),
                "Search added or removed owned entries");
        for (const auto& [path, before] : sources) require(stamp(path) == before,
            "Search changed source identity, content, attributes or timestamps");
    }
};

// The real Search Folder enumerates only this explicit owned scope. Validate
// native parent identity before reading a result's filesystem metadata.
std::set<Identity> nativeResults(IShellItem* search, const Fixture& fixture) {
    ComPtr<IShellFolder> folder;
    succeeded(search->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "Bind native live search");
    ComPtr<IEnumIDList> enumerator;
    succeeded(folder->EnumObjects(nullptr, static_cast<SHCONTF>(SHCONTF_FOLDERS | SHCONTF_NONFOLDERS), &enumerator),
              "Enumerate native owned live search");
    std::set<Identity> result;
    while (enumerator) {
        PITEMID_CHILD pidl = nullptr;
        const auto next = enumerator->Next(1, &pidl, nullptr);
        if (next == S_FALSE) break;
        succeeded(next, "Read owned live search result"); require(pidl != nullptr, "Native result has no identity");
        ComPtr<IShellItem> member;
        const auto created = SHCreateItemWithParent(nullptr, folder.Get(), pidl, IID_PPV_ARGS(&member));
        CoTaskMemFree(pidl); succeeded(created, "Resolve owned live search member");
        PWSTR raw = nullptr; const auto named = member->GetDisplayName(SIGDN_FILESYSPATH, &raw);
        fs::path path = raw ? raw : L""; CoTaskMemFree(raw); succeeded(named, "Resolve native result path");
        require(!path.empty(), "Native result has no filesystem path");
        auto parent = item(path.parent_path()); int order = 1;
        succeeded(parent->Compare(fixture.scope.Get(), SICHINT_CANONICAL, &order), "Compare owned result parent identity");
        require(order == 0, "Native result escaped explicit owned scope");
        require(result.insert(stamp(path).identity).second && result.size() <= fixture.sources.size(),
                "Native result duplicates or exceeds the owned source set");
    }
    return result;
}
ComPtr<IShellItem> query(const LiveSearchRequest& request, const Fixture& fixture) {
    require(request.kind == LiveSearchKind::Query, "Origin request must not build a query");
    ComPtr<IShellItem> result;
    succeeded(explorer::createSearchFolder(request.literal, fixture.scope.Get(), &result), "Create real native automatic/explicit query");
    return result;
}
LiveSearchRequest ready(LiveSearchPolicy& policy, std::uint64_t now) {
    auto value = policy.takeReady(now); require(value.has_value(), "Expected ready latest request"); return *value;
}

void debounceAndNativeFinalResults() {
    Fixture fixture; LiveSearchPolicy policy;
    succeeded(policy.userEdited(L"a", 100), "Schedule first typed literal");
    require(!policy.takeReady(349), "Typing dispatched before debounce");
    for (std::uint64_t n = 0; n < 1000; ++n)
        succeeded(policy.userEdited(L"pending-" + std::to_wstring(n), 200 + n), "Replace pending burst literal");
    const std::wstring literal = L"  System.FileName:=\"\u5b50-gamma.txt\"  ";
    succeeded(policy.userEdited(literal, 1200), "Schedule final exact Unicode query");
    require(policy.literal() == literal && policy.committedLiteral().empty() && policy.deadline() == 1450,
            "Pending edit lost literal spelling or prematurely committed history");
    require(!policy.takeReady(1449) && !policy.takeReady(2000, false), "Debounce or pending navigation was ignored");
    const auto request = ready(policy, 2000);
    require(request.literal == literal && !request.explicitSubmit && !policy.takeReady(3000), "Burst produced extra or altered requests");
    auto native = query(request, fixture);
    require(nativeResults(native.Get(), fixture) == fixture.identities({L"\u5b50-gamma.txt"}), "Automatic final query has incorrect native result identities");
    require(policy.finish(request, S_OK) && policy.committedLiteral().empty() && !policy.finish(request, S_OK),
            "Automatic or duplicate completion committed typed history");
    fixture.unchanged();
}
void staleCompletionAndEnterCommit() {
    Fixture fixture; LiveSearchPolicy policy;
    succeeded(policy.userEdited(L"System.FileName:=\"beta.bin\"", 0), "Schedule first native query");
    const auto old = ready(policy, 250); auto oldNative = query(old, fixture);
    succeeded(policy.userEdited(L"System.FileExtension:=\".txt\"", 260), "Type while native query is issued");
    require(nativeResults(oldNative.Get(), fixture) == fixture.identities({L"beta.bin"}), "Intermediate native query is not real");
    require(!policy.current(old) && !policy.finish(old, S_OK) && policy.literal() == L"System.FileExtension:=\".txt\"" && policy.committedLiteral().empty(),
            "Stale completion replaced newer literal or history");
    succeeded(policy.submit(policy.literal(), 261), "Enter submits before debounce");
    const auto entered = ready(policy, 261); auto enteredNative = query(entered, fixture);
    require(entered.explicitSubmit && nativeResults(enteredNative.Get(), fixture) == fixture.identities({L"alpha.txt", L"\u5b50-gamma.txt"}),
            "Enter did not execute the full native query immediately");
    auto forged = entered; forged.literal = L"other";
    require(!policy.finish(forged, S_OK) && policy.committedLiteral().empty(), "Altered completion was accepted");
    require(policy.finish(entered, S_OK) && policy.committedLiteral() == entered.literal, "Successful actual explicit completion did not commit");
    // Enter on the same already shown automatic/explicit text is still an
    // explicit navigation/commit request, rather than a duplicate EN_CHANGE.
    succeeded(policy.submit(entered.literal, 262), "Submit identical literal explicitly");
    require(ready(policy, 262).generation != entered.generation, "Enter on same text was swallowed");
    fixture.unchanged();
}
void busyRetryPreservesOnlyCurrentIntent() {
    Fixture fixture; LiveSearchPolicy policy;
    const std::wstring literal=L"  System.FileName:=\"beta.bin\"  ";
    succeeded(policy.submit(literal,10), "Issue an explicit native query before busy retry");
    const auto first=ready(policy,10);
    require(policy.retry(first,20)&&policy.current(first)&&policy.waiting()&&policy.deadline()==120&&
            policy.literal()==literal&&policy.committedLiteral().empty(), "Busy retry changed current intent or prematurely committed");
    require(!policy.retry(first,21)&&!policy.finish(first,S_OK)&&!policy.takeReady(119)&&!policy.takeReady(120,false),
            "Busy request accepted duplicate completion/retry or bypassed navigation backpressure");
    const auto second=ready(policy,120);
    require(second==first, "Busy retry lost generation, literal or explicit Enter intent");
    auto native=query(second,fixture);
    require(nativeResults(native.Get(),fixture)==fixture.identities({L"beta.bin"})&&policy.finish(second,S_OK)&&
            policy.committedLiteral()==literal&&!policy.retry(second,121), "Retried native query did not commit exactly once");
    succeeded(policy.userEdited(L"System.FileExtension:=\".txt\"",200), "Issue old automatic query");
    const auto obsolete=ready(policy,450);
    succeeded(policy.userEdited(L"System.FileName:=\"alpha.txt\"",451), "Newer edit replaces busy work");
    const auto newerDeadline=policy.deadline();
    require(!policy.retry(obsolete,452)&&policy.deadline()==newerDeadline&&!policy.finish(obsolete,S_OK),
            "Busy retry resurrected stale work or changed the newer debounce");
    const auto newer=ready(policy,701);native=query(newer,fixture);
    require(nativeResults(native.Get(),fixture)==fixture.identities({L"alpha.txt"})&&policy.finish(newer,S_OK)&&
            policy.committedLiteral()==literal, "Automatic retry changed explicit history or native identities");
    policy.escape(800);const auto origin=ready(policy,800);
    require(policy.retry(origin,801)&&ready(policy,901)==origin, "Busy origin restoration lost its exact intent");
    policy.cancel();require(!policy.retry(origin,902)&&!policy.waiting(), "Cancelled origin request was resurrected");
    const auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    succeeded(policy.submit(literal,maximum-20), "Schedule busy retry near tick limit");
    const auto bounded=ready(policy,maximum-20);
    require(policy.retry(bounded,maximum-10)&&policy.deadline()==maximum&&!policy.takeReady(maximum-1)&&
            ready(policy,maximum)==bounded, "Busy retry deadline overflowed or changed intent");
    fixture.unchanged();
}
void clearEscapeProgrammaticAndNavigationCancellation() {
    Fixture fixture; LiveSearchPolicy policy;
    succeeded(policy.userEdited(L"System.Size:>0", 10), "Schedule cancellable native query");
    const auto old = ready(policy, 260); auto native = query(old, fixture);
    succeeded(policy.userEdited(L"", 261), "Clear search text");
    const auto cleared = ready(policy, 261);
    require(cleared.kind == LiveSearchKind::ReturnToOrigin && !cleared.explicitSubmit && policy.literal().empty() && !policy.finish(old, S_OK),
            "Clear retained an old query or delayed origin restoration");
    // A host restores the saved actual origin Shell identity, not a synthesized
    // 'all files' search. Its source membership remains exactly unchanged.
    auto restored = item(fixture.root); int order = 1;
    succeeded(restored->Compare(fixture.scope.Get(), SICHINT_CANONICAL, &order), "Compare restored native origin");
    require(order == 0 && policy.finish(cleared, S_OK) && policy.committedLiteral().empty(), "Clear changed origin identity/history");
    succeeded(policy.userEdited(L"System.Size:>0", 270), "Schedule before Escape");
    policy.escape(271); const auto escaped = ready(policy, 271);
    require(escaped.kind == LiveSearchKind::ReturnToOrigin && policy.literal().empty() && policy.finish(escaped, S_OK), "Escape did not cancel and restore origin");
    succeeded(policy.userEdited(L"System.Size:>0", 280), "Schedule before programmatic synchronization");
    succeeded(policy.replaceText(L"System.FileName:=\"alpha.txt\""), "Synchronize edit programmatically");
    require(!policy.waiting() && !policy.takeReady(10000) && !policy.current(escaped), "Programmatic SetWindowText emitted or retained a query");
    succeeded(policy.userEdited(L"System.Size:>0", 290), "Schedule before external navigation");
    policy.cancel(); require(!policy.takeReady(10000) && !policy.finish(old, S_OK), "External navigation/close did not invalidate work");
    require(nativeResults(native.Get(), fixture) == fixture.identities({L"alpha.txt", L"beta.bin", L"\u5b50-gamma.txt"}),
            "Cancelled native result folder changed actual scope semantics");
    fixture.unchanged();
}
void incompleteFailureBoundsAndPreservation() {
    Fixture fixture; LiveSearchPolicy policy;
    succeeded(policy.submit(L"System.FileName:=\"alpha.txt\"", 0), "Submit initial valid query");
    const auto initial = ready(policy, 0); auto lastValid = query(initial, fixture);
    require(policy.finish(initial, S_OK), "Complete initial query");
    const auto committed = policy.committedLiteral();
    // An incomplete edit must remain literal input. The native parser may
    // repair it; its HRESULT is not assumed. Automatic failures never become
    // a commit or a modal-error request, and leave the prior native view valid.
    const std::wstring incomplete = L"System.Size:>";
    succeeded(policy.userEdited(incomplete, 10), "Retain incomplete AQS literal");
    const auto automatic = ready(policy, 260);
    ComPtr<IShellItem> incompleteFolder;
    const auto parsed = explorer::createSearchFolder(automatic.literal, fixture.scope.Get(), &incompleteFolder);
    std::cout << "INFO: incomplete automatic native parser HRESULT=0x" << std::hex << static_cast<unsigned long>(parsed) << std::dec << '\n';
    require(!automatic.explicitSubmit && policy.finish(automatic, FAILED(parsed) ? parsed : E_INVALIDARG) &&
            policy.committedLiteral() == committed && policy.literal() == incomplete,
            "Automatic parse/navigation failure lost literal or committed history");
    require(nativeResults(lastValid.Get(), fixture) == fixture.identities({L"alpha.txt"}), "Failed automatic request replaced the prior valid native view");
    // Explicit errors are recognized by their request marker, without calling
    // any MessageBox in this headless fixture.
    succeeded(policy.submit(incomplete, 261), "Submit incomplete literal explicitly");
    const auto explicitFailure = ready(policy, 261);
    require(explicitFailure.explicitSubmit && policy.finish(explicitFailure, E_INVALIDARG) && policy.committedLiteral() == committed,
            "Failed explicit query committed history");
    const auto currentLiteral = policy.literal();
    for (const auto& invalid : {std::wstring(L"ab\0cd", 5), std::wstring(1, static_cast<wchar_t>(0xD800)),
                                std::wstring(LiveSearchPolicy::maximumLiteralLength + 1, L'a')}) {
        require(policy.userEdited(invalid, 300) == E_INVALIDARG && policy.submit(invalid, 300) == E_INVALIDARG &&
                policy.replaceText(invalid) == E_INVALIDARG && policy.literal() == currentLiteral && policy.committedLiteral() == committed,
                "Rejected invalid literal changed accepted state");
    }
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    succeeded(policy.userEdited(L"System.Size:>0", maximum - 100), "Schedule near monotonic tick limit");
    require(policy.deadline() == maximum && !policy.takeReady(maximum - 1), "Debounce arithmetic wrapped to an immediate request");
    require(ready(policy, maximum).kind == LiveSearchKind::Query, "Saturated deadline lost query");
    succeeded(policy.userEdited(L" \t\r\n", 400), "Clear with whitespace-only input");
    require(ready(policy, 400).kind == LiveSearchKind::ReturnToOrigin && policy.literal() == L" \t\r\n", "Whitespace clear lost original literal or issued a query");
    fixture.unchanged();
}
}

int main() {
    explorer::PrivateDesktop desktop;
    if (FAILED(desktop.initialize())) { std::cerr << "FAIL: owned private desktop unavailable\n"; return 1; }
    if (FAILED(OleInitialize(nullptr))) { std::cerr << "FAIL: native search STA unavailable\n"; return 1; }
    const DWORD clipboard = GetClipboardSequenceNumber(); int failures = 0;
    for (const auto& [name, test] : std::array<std::pair<const char*, void(*)()>, 5>{{
        {"debounced latest-only literal and real native final identities", &debounceAndNativeFinalResults},
        {"stale actual native completion and immediate explicit commit", &staleCompletionAndEnterCommit},
        {"busy retry preserves only current intent and real native identities", &busyRetryPreservesOnlyCurrentIntent},
        {"clear/Escape origin, programmatic suppression and navigation cancellation", &clearEscapeProgrammaticAndNavigationCancellation},
        {"incomplete AQS, failure preservation and bounded scheduling", &incompleteFailureBoundsAndPreservation}}}) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
    }
    bool unchanged = false, visible = true;
    if (FAILED(desktop.verifyIsolation(&unchanged)) || !unchanged || FAILED(desktop.visibleWindowsOnInputDesktop(visible)) ||
        visible || GetClipboardSequenceNumber() != clipboard) {
        ++failures; std::cerr << "FAIL: live search desktop/clipboard isolation\n";
    }
    OleUninitialize(); std::cout << 5 - std::min(failures, 5) << "/5 headless live search groups passed\n";
    return failures ? 1 : 0;
}
