#include "explorer/saved_search.hpp"
#include "explorer/search.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <array>
#include <compare>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
std::vector<fs::path> ownedRoots;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::ostringstream text; text << message << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ')';
        throw std::runtime_error(text.str());
    }
}
std::string utf8(std::wstring_view text) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(size || text.empty(), "encode test diagnostic");
    std::string result(static_cast<size_t>(size), '\0');
    if (size) require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) == size, "encode UTF-8");
    return result;
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "write fixture");
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary); require(stream.good(), "read fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
fs::path longPath(const fs::path& path) {
    const auto count = GetLongPathNameW(path.c_str(), nullptr, 0);
    require(count != 0 && count <= 32768, "resolve owned native long path");
    std::wstring text(count, L'\0');
    const auto actual = GetLongPathNameW(path.c_str(), text.data(), count);
    require(actual != 0 && actual < count, "read owned native long path");
    text.resize(actual); return text;
}
struct Fixture {
    fs::path root;
    Fixture() {
        GUID id{}; succeeded(CoCreateGuid(&id), "create fixture identifier");
        wchar_t text[40]{}; require(StringFromGUID2(id, text, 40) != 0, "format identifier");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-saved-metadata-資料&-") + text);
        require(fs::create_directory(root), "create fixture directory");
        ownedRoots.push_back(longPath(root));
    }
    ~Fixture() { ownedRoots.pop_back(); std::error_code error; fs::remove_all(root, error); }
};
ComPtr<IShellItem> shellItem(const fs::path& path) {
    ComPtr<IShellItem> item; succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)), "create fixture Shell item"); return item;
}
fs::path filesystemPath(IShellItem* item) {
    PWSTR raw = nullptr; succeeded(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "read native filesystem path");
    require(raw != nullptr, "native filesystem path missing"); fs::path path(raw); CoTaskMemFree(raw); return path;
}
struct Identity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> id{};
    auto operator<=>(const Identity&) const = default;
};
Identity identity(const fs::path& path) {
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open fixture identity");
    FILE_ID_INFO information{};
    const bool ok = GetFileInformationByHandleEx(handle, FileIdInfo, &information, sizeof(information)) != FALSE;
    CloseHandle(handle); require(ok, "read fixture identity");
    Identity result{information.VolumeSerialNumber};
    std::copy(std::begin(information.FileId.Identifier), std::end(information.FileId.Identifier), result.id.begin());
    return result;
}
std::set<Identity> results(IShellItem* search) {
    ComPtr<IShellFolder> folder; succeeded(search->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "bind native search folder");
    ComPtr<IEnumIDList> enumeration;
    const auto hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumeration);
    if (hr == S_FALSE) return {};
    succeeded(hr, "enumerate native search without a window"); require(enumeration != nullptr, "native enumerator missing");
    std::set<Identity> found;
    for (unsigned count = 0;; ++count) {
        require(count < 256, "native search result bound");
        PITEMID_CHILD child = nullptr; ULONG fetched = 0;
        const auto next = enumeration->Next(1, &child, &fetched);
        if (next == S_FALSE) { CoTaskMemFree(child); break; }
        succeeded(next, "fetch native result"); require(child && fetched == 1, "native result identity missing");
        ComPtr<IShellItem> item;
        const auto create = SHCreateItemWithParent(nullptr, folder.Get(), child, IID_PPV_ARGS(&item));
        CoTaskMemFree(child); succeeded(create, "create native result item");
        const auto path = longPath(filesystemPath(item.Get()));
        const auto owned = std::any_of(ownedRoots.begin(), ownedRoots.end(), [&](const fs::path& root) {
            auto prefix = root.native(); prefix += L'\\';
            return path.native().size() > prefix.size() && _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
        });
        require(owned, "native result escaped exclusively owned fixture scope");
        require(found.insert(identity(path)).second, "duplicate native result identity");
    }
    return found;
}
ComPtr<IShellItem> live(const std::wstring& query, IShellItem* scope, bool recursive = true) {
    ComPtr<IShellItem> item; succeeded(explorer::createSearchFolder(query, scope, &item, recursive), "create restored live query"); return item;
}
ComPtr<IShellItem> liveScopes(const std::wstring& query, IShellItemArray* scopes, bool recursive = true) {
    ComPtr<IShellItem> item;
    succeeded(explorer::createSearchFolderForScopes(query, scopes, &item, recursive), "create restored union-scope query");
    return item;
}
ComPtr<IShellItem> liveRules(const std::wstring& query, const std::vector<explorer::SearchScopeRule>& scopes) {
    ComPtr<IShellItem> item;
    succeeded(explorer::createSearchFolderForScopeRules(query, scopes, &item), "create restored scope-rule query");
    return item;
}
ComPtr<IShellItem> saved(const fs::path& path) { return shellItem(path); }
void eventuallyResults(const std::function<std::set<Identity>()>& readResults,
                       const std::set<Identity>& expected, const char* message) {
    const auto started = GetTickCount64();
    for (;;) {
        const auto actual = readResults();
        if (actual == expected) return;
        if (GetTickCount64() - started >= 5000)
            throw std::runtime_error(std::string(message) + " expected=" + std::to_string(expected.size()) +
                                     " actual=" + std::to_string(actual.size()));
        MSG event{};
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
        Sleep(50);
    }
}
void sameScope(IShellItem* actual, IShellItem* expected) {
    require(actual && expected, "restored scope missing");
    int comparison = 1; succeeded(actual->Compare(expected, SICHINT_CANONICAL, &comparison), "compare native scope");
    require(comparison == 0, "restored scope changed Shell identity");
}
void metadataParity() {
    Fixture fixture;
    const auto path = fixture.root / L"scope & 資料"; require(fs::create_directory(path), "create search scope");
    write(path / L"target.txt", "target"); write(path / L"other.txt", std::string(100, 'o'));
    write(path / L"資料 & Unicode.txt", "unicode");
    auto scope = shellItem(path);
    const std::vector<std::wstring> queries{
        L"System.FileName:target", L"System.FileName:=\"資料 & Unicode.txt\"", L"System.FileName:=\"target.txt\"", L"System.FileName:~<\"tar\"",
        L"System.FileName:~>\".txt\"", L"System.FileName:~=\"arg\"", L"System.FileName:~!\"arg\"",
        L"System.FileName:~\"t*.txt\"", L"System.FileName:$=\"target\"", L"System.Size:>=10",
        L"(System.FileName:=\"target.txt\" OR System.FileName:=\"other.txt\") AND NOT System.Size:<1",
        L"System.Kind:=System.Kind#Document", L"System.Kind:=System.Kind#Picture", L"System.Kind:=System.Kind#Music",
        L"System.Kind:=System.Kind#Video", L"System.Kind:=System.Kind#Folder", L"System.Kind:=System.Kind#Program",
        L"System.DateModified:System.StructuredQueryType.DateTime#Today", L"System.DateModified:System.StructuredQueryType.DateTime#Yesterday",
        L"System.DateModified:System.StructuredQueryType.DateTime#ThisWeek", L"System.DateModified:System.StructuredQueryType.DateTime#LastWeek",
        L"System.DateModified:System.StructuredQueryType.DateTime#ThisMonth", L"System.DateModified:System.StructuredQueryType.DateTime#ThisYear",
        L"System.Size:System.Size#Empty", L"System.Size:System.Size#Tiny", L"System.Size:System.Size#Small",
        L"System.Size:System.Size#Medium", L"System.Size:System.Size#Large", L"System.Size:System.Size#Huge", L"System.Size:System.Size#Gigantic",
        L"System.Size:=6", L"System.Size:<>0", L"System.Size:<6", L"System.Size:>6", L"System.Size:<=6"
    };
    unsigned count = 0;
    for (const auto& query : queries) {
        const auto output = fixture.root / (std::to_wstring(++count) + L".search-ms");
        succeeded(explorer::saveSearch(query, scope.Get(), true, output), "save supported metadata fixture");
        explorer::SavedSearchMetadata metadata;
        const auto hr = explorer::readSavedSearch(output, &metadata);
        if (FAILED(hr)) throw std::runtime_error("read saved metadata for " + utf8(query) + " returned " + std::to_string(static_cast<unsigned long>(hr)));
        sameScope(metadata.scope.Get(), scope.Get()); require(metadata.recursive && !metadata.query.empty(), "metadata recursion/query missing");
        const auto expected = results(live(query, scope.Get()).Get());
        if (count <= 11) {
            const auto target = identity(path / L"target.txt"), other = identity(path / L"other.txt"), unicode = identity(path / L"資料 & Unicode.txt");
            const std::vector<std::set<Identity>> known{
                {target}, {unicode}, {target}, {target}, {target, other, unicode}, {target},
                {other, unicode}, {target}, {target}, {other}, {target, other}};
            require(expected == known[count - 1], "native metadata fixture has wrong known membership/cardinality");
        }
        if (results(saved(output).Get()) != expected)
            throw std::runtime_error("saved fixture membership differs from original: " + utf8(query));
        if (results(live(metadata.query, metadata.scope.Get(), metadata.recursive).Get()) != expected)
            throw std::runtime_error("restated query changed fixture membership: " + utf8(query) + " -> " + utf8(metadata.query));
    }
}
void recursiveAndShallow() {
    Fixture fixture;
    const auto path = fixture.root / L"scope"; require(fs::create_directories(path / L"nested"), "create recursive scope");
    const auto direct = path / L"match-direct.txt", nested = path / L"nested" / L"match-nested.txt";
    write(direct, "direct"); write(nested, "nested"); write(fixture.root / L"match-outside.txt", "outside");
    auto scope = shellItem(path);
    for (const bool recursive : {false, true}) {
        const auto output = fixture.root / (recursive ? L"recursive.search-ms" : L"shallow.search-ms");
        succeeded(explorer::saveSearch(L"System.FileName:~\"match-*.txt\"", scope.Get(), recursive, output), "save scoped fixture");
        explorer::SavedSearchMetadata metadata; succeeded(explorer::readSavedSearch(output, &metadata), "restore scoped fixture");
        require(metadata.recursive == recursive, "restored recursion changed"); sameScope(metadata.scope.Get(), scope.Get());
        std::set<Identity> expected{identity(direct)}; if (recursive) expected.insert(identity(nested));
        require(results(live(metadata.query, metadata.scope.Get(), metadata.recursive).Get()) == expected, "restored scope includes outside/deep result or omits owned result");
    }
}
void relativeDateAndThisPc() {
    Fixture fixture;
    const auto path = fixture.root / L"scope"; require(fs::create_directory(path), "create relative scope");
    write(path / L"today.txt", "today"); const auto old = path / L"old.txt"; write(old, "old");
    SYSTEMTIME historic{}; historic.wYear = 2000; historic.wMonth = 1; historic.wDay = 1;
    FILETIME historicFile{}; require(SystemTimeToFileTime(&historic, &historicFile) != FALSE, "make historic time");
    const auto file = CreateFileW(old.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "open historical file");
    const bool changed = SetFileTime(file, nullptr, nullptr, &historicFile) != FALSE; CloseHandle(file); require(changed, "set historic date");
    auto scope = shellItem(path);
    const auto original = fixture.root / L"today.search-ms", resaved = fixture.root / L"today-again.search-ms";
    succeeded(explorer::saveSearch(L"System.DateModified:System.StructuredQueryType.DateTime#Today", scope.Get(), true, original), "save Today");
    explorer::SavedSearchMetadata metadata; succeeded(explorer::readSavedSearch(original, &metadata), "restore unresolved Today");
    succeeded(explorer::saveSearch(metadata.query, metadata.scope.Get(), metadata.recursive, resaved), "resave unresolved Today");
    require(read(original).find("R00UUUUUUUUZDNNU") != std::string::npos && read(resaved).find("R00UUUUUUUUZDNNU") != std::string::npos, "relative Today expression was frozen to an absolute date");
    require(results(live(metadata.query, metadata.scope.Get()).Get()) == std::set<Identity>{identity(path / L"today.txt")}, "restored Today included historic file or omitted current file");
    const auto computerPath = fixture.root / L"computer.search-ms";
    succeeded(explorer::saveSearch(L"System.FileName:=\"資料 & literal\"", nullptr, true, computerPath), "save This PC");
    explorer::SavedSearchMetadata computer; succeeded(explorer::readSavedSearch(computerPath, &computer), "restore This PC without enumeration");
    ComPtr<IShellItem> expected; succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&expected)), "get This PC");
    sameScope(computer.scope.Get(), expected.Get()); require(computer.recursive && !computer.query.empty(), "This PC metadata missing");
}
void genericQueryParity() {
    Fixture fixture;
    const auto path = fixture.root / L"scope";
    require(fs::create_directories(path / L"nested"), "create generic scope");
    const auto direct = path / L"target.txt", deep = path / L"nested" / L"target.txt";
    const auto prefix = path / L"targetextended.txt", infix = path / L"atarget.txt";
    const auto phrase = path / L"target phrase.txt", separated = path / L"target middle phrase.txt";
    const auto phrasePrefix = path / L"target phraseextended.txt", firstPrefix = path / L"targetextended phrase.txt";
    const auto reversed = path / L"phrase target.txt", unicode = path / L"資料 target.txt";
    const auto other = path / L"other.txt", numeric = path / L"123.txt", sized = path / L"sized.txt";
    const auto fractional = path / L"1.5.txt", boolean = path / L"true.txt", todayName = path / L"today.txt";
    for (const auto& file : {direct, deep, prefix, infix, phrase, separated, phrasePrefix, firstPrefix, reversed, unicode,
                            other, numeric, fractional, boolean, todayName})
        write(file, "neutral");
    write(sized, std::string(123, 'n'));
    const auto historic = path / L"targethistoric.txt"; write(historic, "neutral");
    SYSTEMTIME date{}; date.wYear = 2000; date.wMonth = 1; date.wDay = 1;
    FILETIME timestamp{}; require(SystemTimeToFileTime(&date, &timestamp) != FALSE, "make generic historical timestamp");
    const auto file = CreateFileW(historic.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "open generic historical file");
    const bool changed = SetFileTime(file, nullptr, nullptr, &timestamp) != FALSE;
    CloseHandle(file); require(changed, "set generic historical timestamp");
    auto scope = shellItem(path);
    const std::set<Identity> prefixes{identity(direct), identity(deep), identity(prefix), identity(phrase),
                                    identity(separated), identity(phrasePrefix), identity(firstPrefix),
                                    identity(reversed), identity(unicode), identity(historic)};
    const std::vector<std::wstring> queries{
        L"target", L"\"target\"", L"\"target phrase\"", L"target OR other", L"NOT target",
        L"target AND System.Size:>1", L"target OR System.FileName:=\"other.txt\"",
        L"target AND System.DateModified:System.StructuredQueryType.DateTime#Today",
        L"(target OR other) AND NOT System.DateModified:System.StructuredQueryType.DateTime#Yesterday",
        L"123", L"target OR 123", L"\"資料\" AND target", L"target AND NOT \"middle phrase\"",
        L"target phrase", L"\"target phrase\" OR 123", L"1.5", L"123..124", L">=123", L"true", L"today",
        L"System.Generic.String:$=\"target\"", L"System.Generic.String:$<\"target\"", L"System.FileName:$=\"target\""};
    unsigned count = 0;
    for (const auto& query : queries) {
        const auto output = fixture.root / (L"generic-" + std::to_wstring(++count) + L".search-ms");
        succeeded(explorer::saveSearch(query, scope.Get(), true, output), "save generic fixture");
        const auto expected = results(live(query, scope.Get()).Get());
        require(!expected.empty(), "generic fixture did not exercise native results");
        if (query == L"target" || query == L"System.Generic.String:$<\"target\"")
            require(expected == prefixes, "live generic prefix fixture membership differs");
        if (query == L"\"target\"" || query == L"System.Generic.String:$=\"target\"" || query == L"System.FileName:$=\"target\"")
            require(expected == std::set<Identity>{identity(direct), identity(deep), identity(phrase),
                    identity(separated), identity(phrasePrefix), identity(reversed), identity(unicode)},
                    "quoted generic word changed whole-word boundaries");
        // Windows 10's unindexed generic provider matches both complete words
        // in these filenames, including separated/reversed words. Preserve its
        // actual behavior instead of imposing a different phrase interpretation.
        if (query == L"\"target phrase\"") require(expected == std::set<Identity>{identity(phrase), identity(separated), identity(reversed)},
                    "live generic multiword fixture changed native word membership");
        if (query == L"target AND System.DateModified:System.StructuredQueryType.DateTime#Today") {
            auto today = prefixes; today.erase(identity(historic));
            require(expected == today, "generic Today fixture has wrong known membership");
            require(read(output).find("R00UUUUUUUUZDNNU") != std::string::npos,
                    "generic leaf resolution froze a relative-date sibling");
        }
        if (results(saved(output).Get()) != expected)
            throw std::runtime_error("generic saved query changed native identity membership: " + utf8(query));
        explorer::SavedSearchMetadata metadata;
        const auto hr = explorer::readSavedSearch(output, &metadata);
        if (FAILED(hr)) throw std::runtime_error("read generic saved query failed: " + utf8(query) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        sameScope(metadata.scope.Get(), scope.Get()); require(metadata.recursive, "generic restored recursion differs");
        if (results(live(metadata.query, metadata.scope.Get()).Get()) != expected)
            throw std::runtime_error("generic restored query changed native identities: " + utf8(query) + " -> " + utf8(metadata.query));
        const auto again = fixture.root / (L"generic-resaved-" + std::to_wstring(count) + L".search-ms");
        succeeded(explorer::saveSearch(metadata.query, metadata.scope.Get(), metadata.recursive, again), "resave generic fixture");
        if (results(saved(again).Get()) != expected)
            throw std::runtime_error("resaved generic query changed native identities: " + utf8(query) + " -> " + utf8(metadata.query));
    }
    const auto shallow = fixture.root / L"generic-shallow.search-ms";
    succeeded(explorer::saveSearch(L"target", scope.Get(), false, shallow), "save shallow generic fixture");
    auto expected = prefixes; expected.erase(identity(deep));
    require(results(saved(shallow).Get()) == expected, "shallow generic saved query changed scope membership");
    explorer::SavedSearchMetadata metadata;
    succeeded(explorer::readSavedSearch(shallow, &metadata), "restore shallow generic fixture");
    require(!metadata.recursive && results(live(metadata.query, metadata.scope.Get(), false).Get()) == expected,
            "restored shallow generic query includes nested or outside items");
    const auto added = path / L"targetnew.txt", outside = fixture.root / L"target-outside.txt";
    write(added, "neutral"); write(outside, "neutral");
    expected.insert(identity(added));
    require(results(saved(shallow).Get()) == expected, "saved generic query froze results or included outside scope");
    auto recursiveExpected = prefixes; recursiveExpected.insert(identity(added));
    require(results(saved(fixture.root / L"generic-1.search-ms").Get()) == recursiveExpected,
            "recursive saved generic query did not discover a newly matching identity");
    fs::rename(added, path / L"newly-unmatched.txt");
    expected.erase(identity(path / L"newly-unmatched.txt"));
    require(results(saved(shallow).Get()) == expected, "saved generic query retained a renamed nonmatching identity");
}
void ownedLibraryScopeParity() {
    Fixture fixture;
    const auto first = fixture.root / L"first 資料", second = fixture.root / L"second";
    require(fs::create_directories(first / L"nested") && fs::create_directory(second), "create owned library scope folders");
    const auto direct = first / L"target.txt", peer = second / L"target.txt", deep = first / L"nested" / L"target.txt";
    for (const auto& path : {direct, peer, deep, fixture.root / L"target-outside.txt"}) write(path, "neutral");
    auto firstItem = shellItem(first), secondItem = shellItem(second), ownedRoot = shellItem(fixture.root);
    ComPtr<IShellLibrary> library;
    succeeded(CoCreateInstance(CLSID_ShellLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&library)), "create owned native library");
    succeeded(library->AddFolder(firstItem.Get()), "include first owned library location");
    succeeded(library->AddFolder(secondItem.Get()), "include second owned library location");
    succeeded(library->SetDefaultSaveFolder(DSFT_PRIVATE, firstItem.Get()), "set owned default library location");
    ComPtr<IShellItem> scope;
    succeeded(library->Save(ownedRoot.Get(), L"Owned search 資料", LSF_FAILIFTHERE, &scope), "save library exclusively inside fixture");
    require(fs::equivalent(filesystemPath(scope.Get()), fixture.root / L"Owned search 資料.library-ms"), "library Save escaped exclusive owned root");
    library.Reset();
    const std::set<Identity> expected{identity(direct), identity(peer), identity(deep)};
    eventuallyResults([&] { return results(live(L"target", scope.Get()).Get()); }, expected,
                      "native library search scope changed owned identities");
    const auto output = fixture.root / L"library.search-ms";
    succeeded(explorer::saveSearch(L"target", scope.Get(), true, output), "save native library scoped search");
    require(results(saved(output).Get()) == expected, "native saved library scope changed location membership");
    explorer::SavedSearchMetadata metadata;
    succeeded(explorer::readSavedSearch(output, &metadata), "restore native library search metadata");
    DWORD count = 0; require(metadata.scopes != nullptr, "restored union scope missing");
    succeeded(metadata.scopes->GetCount(&count), "read restored library scope count");
    require(count == 2 && metadata.recursive, "restored library locations/recursion changed");
    ComPtr<IShellItem> restoredFirst, restoredSecond;
    succeeded(metadata.scopes->GetItemAt(0, &restoredFirst), "read first restored library location");
    succeeded(metadata.scopes->GetItemAt(1, &restoredSecond), "read second restored library location");
    sameScope(restoredFirst.Get(), firstItem.Get()); sameScope(restoredSecond.Get(), secondItem.Get());
    require(results(liveScopes(metadata.query, metadata.scopes.Get()).Get()) == expected, "restored library query changed exact identities");
    const auto again = fixture.root / L"library-again.search-ms";
    succeeded(explorer::saveSearchForScopes(metadata.query, metadata.scopes.Get(), true, again), "resave owned library search");
    require(results(saved(again).Get()) == expected, "resaved library search changed native identity membership");
    const auto shallow = fixture.root / L"library-shallow.search-ms";
    succeeded(explorer::saveSearch(L"target", scope.Get(), false, shallow), "save shallow library search");
    const std::set<Identity> directExpected{identity(direct), identity(peer)};
    require(results(live(L"target", scope.Get(), false).Get()) == directExpected, "shallow live library scope changed root membership");
    require(results(saved(shallow).Get()) == directExpected, "shallow saved library scope includes descendants/outside or loses a location");
    explorer::SavedSearchMetadata shallowMetadata;
    succeeded(explorer::readSavedSearch(shallow, &shallowMetadata), "restore shallow library search");
    require(!shallowMetadata.recursive && results(liveScopes(shallowMetadata.query, shallowMetadata.scopes.Get(), false).Get()) == directExpected,
            "restored shallow library union changed identity membership");
    const auto added = second / L"targetnew.txt"; write(added, "neutral");
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, added.c_str(), nullptr);
    auto updated = expected; updated.insert(identity(added));
    eventuallyResults([&] { return results(saved(output).Get()); }, updated,
                      "saved library search did not discover new location content");
}
void literalPercentAndKnownFolderScopes() {
    Fixture fixture;
    const auto percent = fixture.root / L"100% complete";
    require(fs::create_directory(percent), "create literal percent scope");
    const auto target = percent / L"target.txt"; write(target, "neutral");
    auto scope = shellItem(percent);
    const auto output = fixture.root / L"literal-percent.search-ms";
    succeeded(explorer::saveSearch(L"target", scope.Get(), true, output), "save non-expanding literal percent scope");
    const std::set<Identity> expected{identity(target)};
    require(results(saved(output).Get()) == expected, "native saved percent scope changed owned identity");
    explorer::SavedSearchMetadata metadata;
    succeeded(explorer::readSavedSearch(output, &metadata), "restore literal percent scope");
    sameScope(metadata.scope.Get(), scope.Get());
    require(results(liveScopes(metadata.query, metadata.scopes.Get()).Get()) == expected,
            "restored percent scope changed native result identity");
    unsigned count = 0;
    for (const auto* identifier : {&FOLDERID_NetworkFolder, &FOLDERID_ControlPanelFolder}) {
        ComPtr<IShellItem> known;
        succeeded(SHGetKnownFolderItem(*identifier, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&known)), "read existing virtual known folder");
        const auto file = fixture.root / (L"known-" + std::to_wstring(++count) + L".search-ms");
        succeeded(explorer::saveSearch(L"System.FileName:=\"owned-no-enumeration\"", known.Get(), true, file), "save exact virtual known-folder scope");
        wchar_t text[40]{}; require(StringFromGUID2(*identifier, text, 40) != 0, "format known scope GUID");
        require(read(file).find("knownFolder=\"" + utf8(text) + "\"") != std::string::npos,
                "virtual scope did not persist its exact known-folder identity");
        explorer::SavedSearchMetadata restored;
        succeeded(explorer::readSavedSearch(file, &restored), "restore virtual known-folder metadata");
        sameScope(restored.scope.Get(), known.Get());
        DWORD locations = 0; succeeded(restored.scopes->GetCount(&locations), "read known-folder scope count");
        require(locations == 1 && restored.recursive, "known-folder scope count or recursion changed");
        // Parse/PIDL-round-trip only. Never enumerate a user-wide or network
        // scope while verifying metadata for an owned saved-search file.
        auto native = saved(file);
        SFGAOF attributes = 0;
        succeeded(native->GetAttributes(SFGAO_FOLDER, &attributes), "read saved virtual-scope attributes");
        require((attributes & SFGAO_FOLDER) != 0, "native saved virtual scope did not reopen as a folder");
    }
}
void includeExcludeAndMixedScopeRules() {
    Fixture fixture;
    const auto scope = fixture.root / L"scope", first = scope / L"first", second = scope / L"second";
    const auto excluded = second / L"target-excluded";
    require(fs::create_directories(first / L"deep") && fs::create_directories(second / L"deep") &&
            fs::create_directories(excluded / L"nested"), "create rule fixture locations");
    const auto firstDirect = first / L"target.txt", firstDeep = first / L"deep" / L"target.txt";
    const auto secondDirect = second / L"target.txt", secondDeep = second / L"deep" / L"target.txt";
    const auto excludedDirect = excluded / L"target.txt", excludedDeep = excluded / L"nested" / L"target.txt";
    for (const auto& path : {firstDirect, firstDeep, secondDirect, secondDeep, excludedDirect, excludedDeep,
                            fixture.root / L"target-outside.txt"}) write(path, "neutral");
    auto rootItem = shellItem(scope), firstItem = shellItem(first), secondItem = shellItem(second), excludedItem = shellItem(excluded);
    using Rule = explorer::SearchScopeRule;
    struct Case { std::vector<Rule> rules; std::set<Identity> expected; };
    const std::set<Identity> allSecond{identity(secondDirect), identity(secondDeep), identity(excluded), identity(excludedDirect), identity(excludedDeep)};
    auto mixed = allSecond; mixed.insert(identity(firstDirect));
    const std::vector<Case> cases{
        {{{firstItem, false, false}, {secondItem, true, false}}, mixed},
        {{{firstItem, false, false}, {secondItem, true, false}, {excludedItem, true, true}},
            {identity(firstDirect), identity(secondDirect), identity(secondDeep)}},
        {{{rootItem, true, false}, {firstItem, true, true}, {excludedItem, true, true}},
            {identity(secondDirect), identity(secondDeep)}},
        {{{rootItem, true, false}, {firstItem, true, false}, {excludedItem, true, true}},
            {identity(firstDirect), identity(firstDeep), identity(secondDirect), identity(secondDeep)}}
    };
    unsigned count = 0;
    for (const auto& example : cases) {
        const auto file = fixture.root / (L"rules-" + std::to_wstring(++count) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(L"target", example.rules, file), "save explicit scope rules");
        const auto native = results(saved(file).Get());
        if (native != example.expected) throw std::runtime_error("native include/exclude fixture case " + std::to_string(count) +
            " has unexpected identities expected=" + std::to_string(example.expected.size()) + " actual=" + std::to_string(native.size()) +
            " firstDirect=" + std::to_string(native.contains(identity(firstDirect))) + " firstDeep=" + std::to_string(native.contains(identity(firstDeep))) +
            " secondDirect=" + std::to_string(native.contains(identity(secondDirect))) + " secondDeep=" + std::to_string(native.contains(identity(secondDeep))) +
            " excludedFolder=" + std::to_string(native.contains(identity(excluded))) + " excludedDirect=" + std::to_string(native.contains(identity(excludedDirect))) +
            " excludedDeep=" + std::to_string(native.contains(identity(excludedDeep))));
        require(results(liveRules(L"target", example.rules).Get()) == example.expected, "live rule conditions differ from native scope XML");
        explorer::SavedSearchMetadata restored;
        succeeded(explorer::readSavedSearch(file, &restored), "restore include/exclude rules");
        require(restored.scopeRules.size() == example.rules.size(), "scope-rule import lost locations");
        for (size_t i = 0; i < example.rules.size(); ++i) {
            sameScope(restored.scopeRules[i].folder.Get(), example.rules[i].folder.Get());
            require(restored.scopeRules[i].recursive == example.rules[i].recursive &&
                    restored.scopeRules[i].excluded == example.rules[i].excluded, "scope-rule import changed recursion/exclusion");
        }
        require(results(liveRules(restored.query, restored.scopeRules).Get()) == example.expected,
                "restored scope-rule query changed native identity membership");
        const auto again = fixture.root / (L"rules-again-" + std::to_wstring(count) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(restored.query, restored.scopeRules, again), "resave scope-rule query");
        require(results(saved(again).Get()) == example.expected, "resaved scope rules changed native identities");
    }
    const auto untouched = fixture.root / L"rules-no-output.search-ms";
    require(explorer::saveSearchForScopeRules(L"target", {{firstItem, true, true}}, untouched) == unsupported && !fs::exists(untouched),
            "exclude-only scope created an invalid saved search");
    require(explorer::saveSearchForScopeRules(L"target", std::vector<Rule>(257, {firstItem, true, false}), untouched) == unsupported && !fs::exists(untouched),
            "oversized scope rules created a file");
    require(explorer::saveSearchForScopeRules(L"target", {{nullptr, true, false}}, untouched) == E_INVALIDARG && !fs::exists(untouched),
            "null scope rule created a file");
    require(explorer::saveSearchForScopeRules(L"target", {{secondItem, true, false}, {excludedItem, false, true}}, untouched) == unsupported && !fs::exists(untouched),
            "unverified shallow-exclude membership created an editable file");
    require(explorer::saveSearchForScopeRules(L"target", {{firstItem, true, false}, {firstItem, true, true}}, untouched) == unsupported && !fs::exists(untouched),
            "unverified same-root exclusion created an editable file");
    require(explorer::saveSearchForScopeRules(L"target", {{secondItem, false, false}, {excludedItem, true, true}}, untouched) == unsupported && !fs::exists(untouched),
            "unverified excluded child of shallow root created an editable file");
    const auto shallowExclude = fixture.root / L"native-only-shallow-exclude.search-ms";
    auto shallowXml = read(fixture.root / L"rules-2.search-ms");
    const auto excludeAt = shallowXml.find("<exclude");
    const auto recursionAt = shallowXml.find("nonRecursive=\"false\"", excludeAt);
    require(excludeAt != std::string::npos && recursionAt != std::string::npos, "external shallow-exclude fixture marker absent");
    shallowXml.replace(recursionAt, std::string("nonRecursive=\"false\"").size(), "nonRecursive=\"true\"");
    write(shallowExclude, shallowXml);
    explorer::SavedSearchMetadata rejected;
    require(explorer::readSavedSearch(shallowExclude, &rejected) == unsupported,
            "provider-dependent external shallow exclusion became an editable rule");
    auto nativeOnly = saved(shallowExclude);
    SFGAOF flags = 0;
    succeeded(nativeOnly->GetAttributes(SFGAO_FOLDER, &flags), "open native-only external shallow exclusion");
    require((flags & SFGAO_FOLDER) != 0, "external shallow exclusion stopped opening through the native provider");
    succeeded(explorer::saveSearchForScopeRules(L"target", cases.front().rules, untouched), "create exclusive rule output");
    const auto before = read(untouched);
    const auto beforeId = identity(untouched);
    require(FAILED(explorer::saveSearchForScopeRules(L"other", cases.back().rules, untouched)) &&
            read(untouched) == before && identity(untouched) == beforeId, "rule save overwrote existing file/identity");
}
void nativeKindUnionMetadataParity() {
    Fixture fixture;
    const auto path = fixture.root / L"scope", folder = path / L"target-folder";
    require(fs::create_directories(folder), "create native kind fixture scope");
    const auto document = path / L"target-document.txt", nested = folder / L"target-nested.txt", picture = path / L"target-picture.bmp";
    write(document, "neutral"); write(nested, "neutral");
    BITMAPFILEHEADER fileHeader{}; fileHeader.bfType = 0x4D42; fileHeader.bfSize = 58; fileHeader.bfOffBits = 54;
    BITMAPINFOHEADER imageHeader{}; imageHeader.biSize = sizeof(imageHeader); imageHeader.biWidth = 1;
    imageHeader.biHeight = 1; imageHeader.biPlanes = 1; imageHeader.biBitCount = 24; imageHeader.biSizeImage = 4;
    std::string bitmap(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    bitmap.append(reinterpret_cast<const char*>(&imageHeader), sizeof(imageHeader)); bitmap.append("\x20\x40\x80\0", 4);
    write(picture, bitmap);
    auto scope = shellItem(path);
    const auto base = fixture.root / L"kind-base.search-ms";
    succeeded(explorer::saveSearch(L"target", scope.Get(), true, base), "save native kind fixture base");
    const auto original = read(base);
    struct Case { const char* kinds; std::set<Identity> expected; };
    const std::vector<Case> cases{
        {"<kind name=\"document\"/><kind name=\"picture\"/>", {identity(document), identity(nested), identity(picture)}},
        {"<kind name=\"Picture\"/><kind name=\"Folder\"/>", {identity(picture), identity(folder)}},
        {"<kind name=\"folder\"/>", {identity(folder)}},
        {"<kind name=\"document\"/><kind name=\"document\"/>", {identity(document), identity(nested)}},
        {"<kind name=\"item\"/><kind name=\"picture\"/>", {identity(picture)}}
    };
    unsigned count = 0;
    for (const auto& example : cases) {
        auto xml = original; const auto marker = xml.find("<kind name=\"item\"/>");
        require(marker != std::string::npos, "native kind fixture marker missing");
        xml.replace(marker, std::string("<kind name=\"item\"/>").size(), example.kinds);
        const auto file = fixture.root / (L"kind-" + std::to_wstring(++count) + L".search-ms");
        write(file, xml);
        const auto native = results(saved(file).Get());
        if (native != example.expected) throw std::runtime_error("native kind-union fixture case " + std::to_string(count) +
            " changed known membership expected=" + std::to_string(example.expected.size()) + " actual=" + std::to_string(native.size()) +
            " document=" + std::to_string(native.contains(identity(document))) + " nested=" + std::to_string(native.contains(identity(nested))) +
            " picture=" + std::to_string(native.contains(identity(picture))) + " folder=" + std::to_string(native.contains(identity(folder))));
        explorer::SavedSearchMetadata metadata;
        succeeded(explorer::readSavedSearch(file, &metadata), "restore native kind union");
        const auto restored = results(liveRules(metadata.query, metadata.scopeRules).Get());
        if (restored != example.expected) throw std::runtime_error("native parser kind union case " + std::to_string(count) +
            " changed restored identities expected=" + std::to_string(example.expected.size()) + " actual=" + std::to_string(restored.size()) +
            " query=" + utf8(metadata.query));
        const auto again = fixture.root / (L"kind-again-" + std::to_wstring(count) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(metadata.query, metadata.scopeRules, again), "resave native kind-union query");
        require(results(saved(again).Get()) == example.expected, "resaving native kind union discarded/broadened its filter");
    }
}
void unchangedOnFailure(const fs::path& file, IShellItem* sentinel, HRESULT expected = S_OK) {
    explorer::SavedSearchMetadata output{L"unchanged query", sentinel, false};
    succeeded(SHCreateShellItemArrayFromShellItem(sentinel, IID_PPV_ARGS(&output.scopes)), "create metadata output sentinel union");
    output.scopeRules = {{sentinel, false, false}, {sentinel, true, true}};
    const auto* originalUnion = output.scopes.Get();
    const auto hr = explorer::readSavedSearch(file, &output);
    require(FAILED(hr), "unsupported/malformed saved metadata accepted");
    if (expected != S_OK && hr != expected)
        throw std::runtime_error("unexpected metadata rejection result for " + utf8(file.filename().native()) + ": " + std::to_string(static_cast<unsigned long>(hr)));
    require(output.query == L"unchanged query" && !output.recursive && output.scope.Get() == sentinel &&
            output.scopes.Get() == originalUnion && output.scopeRules.size() == 2 &&
            output.scopeRules[0].folder.Get() == sentinel && !output.scopeRules[0].recursive && !output.scopeRules[0].excluded &&
            output.scopeRules[1].folder.Get() == sentinel && output.scopeRules[1].recursive && output.scopeRules[1].excluded,
            "failed metadata read changed caller output fields");
}
void unsupportedExternalShapes() {
    Fixture fixture; auto scope = shellItem(fixture.root);
    const auto base = fixture.root / L"base.search-ms";
    succeeded(explorer::saveSearch(L"System.Size:>1", scope.Get(), true, base), "save rejection base");
    const auto original = read(base);
    auto changed = [&](const std::string& before, const std::string& after, unsigned index) {
        auto bytes = original; const auto at = bytes.find(before); require(at != std::string::npos, "rejection fixture marker absent"); bytes.replace(at, before.size(), after);
        const auto output = fixture.root / (L"external-" + std::to_wstring(index) + L".search-ms"); write(output, bytes); unchangedOnFailure(output, scope.Get(), unsupported);
    };
    changed("nonRecursive=\"false\"", "nonRecursive=\"unsupported\"", 1);
    changed("<include", "<exclude", 2);
    changed("<query>", "<query><providers/>", 3);
    changed("<query>", "<query><subQueries/>", 4);
    changed("name=\"item\"", "name=\"unsupported-kind\"", 5);
    changed("propertyType=\"wstr\"", "propertyType=\"integer\"", 6);
    changed("operator=\"gt\"", "operator=\"wordstarts with\"", 7);
    changed("<conditions>", "<conditions><attributes/>", 8);
    changed("persistedQuery version=\"1.0\"", "persistedQuery version=\"2.0\"", 9);
    changed("<query>", "<query xmlns=\"urn:unsupported\">", 10);
    changed("</kindList>", "<kind name=\"unsupported-kind\"/></kindList>", 11);
    changed("</kindList>", "<kind name=\"item\"/></kindList>", 13);
    std::string tooManyKinds;
    for (unsigned i = 0; i < 64; ++i) tooManyKinds += "<kind name=\"document\"/>";
    changed("</kindList>", tooManyKinds + "</kindList>", 14);
    // A plain external semantic value that cannot round-trip as a structured
    // unresolved token must be rejected instead of silently changing meaning.
    changed("valuetype=\"System.StructuredQueryType.Integer\"", "valuetype=\"Unknown.External.Type\"", 12);
    // Reopening old files with unspecified generic leaves cannot faithfully
    // restore their parser-dependent comparison; keep them out of host edits.
    changed("property=\"System.Size\"", "", 13);
    changed("nonRecursive=\"false\"", "nonRecursive=\"1\"", 14);
    const auto scopeStart = original.find("<scope>"), scopeEnd = original.find("</scope>");
    require(scopeStart != std::string::npos && scopeEnd != std::string::npos, "knownFolder rejection marker missing");
    auto progId = original;
    progId.replace(scopeStart, scopeEnd + 8 - scopeStart, "<scope><include knownFolder=\"windows-explorer-metadata-unsupported-ProgID\" nonRecursive=\"false\"/></scope>");
    const auto progIdPath = fixture.root / L"unsupported-progid.search-ms";
    write(progIdPath, progId); unchangedOnFailure(progIdPath, scope.Get(), unsupported);
    const auto blurb = fixture.root / L"unsupported-blurb.search-ms";
    succeeded(explorer::saveSearch(L"System.Title:=\"猫 & \"\"quoted\"\" < > ' \U0001F680\"", scope.Get(), true, blurb), "save opaque Blurb fixture");
    unchangedOnFailure(blurb, scope.Get(), unsupported);
}
void malformedBoundsAndDtd() {
    Fixture fixture; auto scope = shellItem(fixture.root);
    const auto output = fixture.root / L"bad.search-ms";
    for (const auto& bytes : {std::string(), std::string("<persistedQuery>"), std::string("not XML"),
         std::string("<?xml version=\"1.0\"?><!DOCTYPE persistedQuery [<!ENTITY small \"safe fixture\">]><persistedQuery version=\"1.0\">&small;</persistedQuery>"),
         std::string("<?xml version=\"1.0\"?><!DOCTYPE persistedQuery SYSTEM \"file:///never-read-fixture.dtd\"><persistedQuery version=\"1.0\"/>"),
         std::string(1024 * 1024 + 1, 'x')}) {
        write(output, bytes); unchangedOnFailure(output, scope.Get());
    }
    std::string deep = "<persistedQuery version=\"1.0\">";
    for (unsigned i = 0; i < 100; ++i) deep += "<viewInfo>";
    for (unsigned i = 0; i < 100; ++i) deep += "</viewInfo>";
    deep += "</persistedQuery>"; write(output, deep); unchangedOnFailure(output, scope.Get());
    std::string many = "<persistedQuery version=\"1.0\"><viewInfo>";
    for (unsigned i = 0; i < 4200; ++i) many += "<column/>";
    many += "</viewInfo></persistedQuery>"; write(output, many); unchangedOnFailure(output, scope.Get(), unsupported);
    require(explorer::readSavedSearch(output, nullptr) == E_POINTER, "null metadata output accepted");
    unchangedOnFailure({}, scope.Get(), E_INVALIDARG);
    unchangedOnFailure(fixture.root / L"wrong.txt", scope.Get(), E_INVALIDARG);
    unchangedOnFailure(fixture.root / L"missing.search-ms", scope.Get(), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
}
} // namespace

int runSavedSearchTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"saved metadata native query/scope/result parity", metadataParity},
        {"generic prefix/phrase/numeric/Boolean/date native saved identity parity", genericQueryParity},
        {"owned native Library live/saved/restored scope identity parity", ownedLibraryScopeParity},
        {"literal percent native results and virtual known-folder metadata identity", literalPercentAndKnownFolderScopes},
        {"native include/exclude/mixed recursion rules and lossless re-save identities", includeExcludeAndMixedScopeRules},
        {"native kind union imported/restated/re-saved exact identity parity", nativeKindUnionMetadataParity},
        {"saved metadata shallow/recursive exact fixture identities", recursiveAndShallow},
        {"saved metadata unresolved relative dates and This PC", relativeDateAndThisPc},
        {"saved metadata unsupported external shapes and output preservation", unsupportedExternalShapes},
        {"saved metadata malformed/oversize/deep/node limits and DTD prohibition", malformedBoundsAndDtd}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    std::cout << tests.size() - static_cast<size_t>(failures) << '/' << tests.size() << " headless saved-search metadata groups passed\n";
    return failures;
}
