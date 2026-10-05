#include "explorer/saved_search.hpp"
#include "explorer/search.hpp"
#include "explorer/live_search.hpp"
#include "explorer/typed_address.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <propsys.h>
#include <structuredquery.h>
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
fs::path extendedPath(const fs::path& path) {
    const auto& value = path.native();
    if (!path.is_absolute() || value.starts_with(L"\\\\?\\")) return path;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(extendedPath(path), std::ios::binary); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "write fixture");
}
std::string read(const fs::path& path) {
    std::ifstream stream(extendedPath(path), std::ios::binary); require(stream.good(), "read fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
fs::path longPath(const fs::path& path) {
    const auto native = extendedPath(path);
    const auto count = GetLongPathNameW(native.c_str(), nullptr, 0);
    require(count != 0 && count <= 32768, "resolve owned native long path");
    std::wstring text(count, L'\0');
    const auto actual = GetLongPathNameW(native.c_str(), text.data(), count);
    require(actual != 0 && actual < count, "read owned native long path");
    text.resize(actual);
    if (text.starts_with(L"\\\\?\\UNC\\")) text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    return text;
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
    ~Fixture() { ownedRoots.pop_back(); std::error_code error; fs::remove_all(extendedPath(root), error); }
};
ComPtr<IShellItem> shellItem(const fs::path& path) {
    // Extended prefixes are Win32 file-I/O syntax, not Shell parsing names.
    // Our owned hierarchy has ordinary component names; use the corresponding
    // Shell name while retaining extended prefixes for all long file I/O.
    auto parsing = path.native();
    if (parsing.starts_with(L"\\\\?\\UNC\\")) parsing = L"\\\\" + parsing.substr(8);
    else if (parsing.starts_with(L"\\\\?\\")) parsing.erase(0, 4);
    ComPtr<IShellItem> item; succeeded(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item)), "create fixture Shell item"); return item;
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
    const auto native = extendedPath(path);
    const auto handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
    output.fileProperties.emplace(); output.fileProperties->description = L"unchanged file metadata";
    const auto* originalUnion = output.scopes.Get();
    const auto hr = explorer::readSavedSearch(file, &output);
    require(FAILED(hr), "unsupported/malformed saved metadata accepted");
    if (expected != S_OK && hr != expected)
        throw std::runtime_error("unexpected metadata rejection result for " + utf8(file.filename().native()) + ": " + std::to_string(static_cast<unsigned long>(hr)));
    require(output.query == L"unchanged query" && !output.recursive && output.scope.Get() == sentinel &&
            output.scopes.Get() == originalUnion && output.scopeRules.size() == 2 &&
            output.scopeRules[0].folder.Get() == sentinel && !output.scopeRules[0].recursive && !output.scopeRules[0].excluded &&
            output.scopeRules[1].folder.Get() == sentinel && output.scopeRules[1].recursive && output.scopeRules[1].excluded &&
            output.fileProperties && output.fileProperties->description == L"unchanged file metadata",
            "failed metadata read changed caller output fields");
}
void protectedPhysicalScopeParity() {
    Fixture fixture;
    const auto rootPath = fixture.root / L"protected scope";
    const auto childPath = rootPath / L"target-child", deepPath = childPath / L"target-deep";
    const auto peerPath = fixture.root / L"peer scope";
    require(fs::create_directories(deepPath) && fs::create_directory(peerPath), "create owned protective scope hierarchy");
    const auto direct = rootPath / L"target-direct.txt", child = childPath / L"target-child.txt";
    const auto deep = deepPath / L"target-deep.txt", ignored = rootPath / L"target-ignored.txt";
    const auto peer = peerPath / L"peer-control.txt", outside = fixture.root / L"target-outside.txt";
    const auto historic = rootPath / L"target-historic.txt";
    for (const auto& path : {direct, child, deep, ignored, peer, outside, historic}) write(path, "unchanged protected scope member");
    SYSTEMTIME old{}; old.wYear = 2000; old.wMonth = 1; old.wDay = 1;
    FILETIME oldTime{}; require(SystemTimeToFileTime(&old, &oldTime) != FALSE, "make protective query old timestamp");
    const auto handle = CreateFileW(extendedPath(historic).c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open only owned protective historical member");
    const bool dated = SetFileTime(handle, nullptr, nullptr, &oldTime) != FALSE;
    CloseHandle(handle); require(dated, "set only owned protective historical timestamp");
    std::vector<std::pair<fs::path, Identity>> originalMembers;
    for (const auto& path : {rootPath, childPath, deepPath, peerPath, direct, child, deep, ignored, peer, outside, historic})
        originalMembers.emplace_back(path, identity(path));
    auto root = shellItem(rootPath), childFolder = shellItem(childPath), peerFolder = shellItem(peerPath);
    const auto directId = identity(direct), childId = identity(child), deepId = identity(deep);
    const auto deepFolderId = identity(deepPath), peerId = identity(peer);
    using Rule = explorer::SearchScopeRule;
    struct Example { const char* name; std::vector<Rule> rules; std::set<Identity> expected; };
    const std::vector<Example> examples{
        {"shallow child exclusion", {{root, true, false}, {childFolder, false, true}}, {directId, deepId}},
        {"recursive equal root", {{root, true, false}, {root, true, true}}, {}},
        {"recursive equal root with surviving peer", {{root, true, false}, {peerFolder, true, false}, {root, true, true}}, {peerId}},
        {"shallow equal root with surviving descendants", {{root, true, false}, {peerFolder, true, false}, {root, false, true}},
            {childId, deepFolderId, deepId, peerId}},
        {"recursive child of shallow root", {{root, false, false}, {peerFolder, true, false}, {childFolder, true, true}}, {directId, peerId}},
        {"shallow child of shallow root", {{root, false, false}, {peerFolder, true, false}, {childFolder, false, true}}, {directId, peerId}}
    };
    const std::wstring query = L"(System.FileName:~\"target*\" OR System.FileName:=\"peer-control.txt\") AND "
        L"NOT System.FileName:=\"target-ignored.txt\" AND System.DateModified:System.StructuredQueryType.DateTime#Today";
    explorer::SearchFileProperties properties;
    properties.author = L"Owned guard author"; properties.description = L"Guard %1\r\n資料 & metadata";
    properties.tags = L"owned protective scope";
    explorer::SearchViewPresentation presentation;
    presentation.mode = explorer::SearchViewMode::Details; presentation.iconSize = 16;
    presentation.visibleColumns = std::vector<std::wstring>{L"System.ItemNameDisplay", L"System.DateModified"};
    const auto scaffoldPath = fixture.root / L"unguarded-query-scaffold.search-ms";
    succeeded(explorer::saveSearch(query, root.Get(), true, scaffoldPath), "save native original protective query scaffold");
    const auto conditionPayload = [](const std::string& xml) {
        const auto start = xml.find("<conditions>"), finish = xml.find("</conditions>");
        require(start != std::string::npos && finish != std::string::npos && finish > start,
                "protective XML condition boundary absent");
        return xml.substr(start + std::string("<conditions>").size(), finish - start - std::string("<conditions>").size());
    };
    const auto unguardedQuery = conditionPayload(read(scaffoldPath));
    const auto replacePayload = [&](std::string xml, const std::string& payload) {
        const auto start = xml.find("<conditions>");
        const auto oldPayload = conditionPayload(xml);
        xml.replace(start + std::string("<conditions>").size(), oldPayload.size(), payload);
        return xml;
    };
    unsigned index = 0;
    for (const auto& example : examples) {
        const auto source = fixture.root / (L"protected-" + std::to_wstring(++index) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(query, example.rules, source, explorer::SearchSaveMode::CreateNew,
                                                   &presentation, &properties), "save canonical protective physical scopes");
        const auto sourceBytes = read(source); const auto sourceId = identity(source);
        const auto checkedResults = [&](const std::function<std::set<Identity>()>& route, const char* stage) {
            try { eventuallyResults(route, example.expected, stage); }
            catch (const std::exception& error) { throw std::runtime_error(std::string(example.name) + ": " + error.what()); }
        };
        checkedResults([&] { return results(liveRules(query, example.rules).Get()); }, "protected live scope changed actual FileIDs");
        checkedResults([&] { return results(saved(source).Get()); }, "protected native XML scope changed actual FileIDs");
        explorer::SavedSearchMetadata metadata;
        succeeded(explorer::readSavedSearch(source, &metadata), "import exact native protective scope guard");
        require(metadata.scopeRules.size() == example.rules.size() && metadata.fileProperties == properties &&
                metadata.presentation && metadata.presentation->mode == presentation.mode &&
                metadata.presentation->iconSize == presentation.iconSize && metadata.presentation->visibleColumns == presentation.visibleColumns,
                "protective import lost original scope/file/view metadata");
        for (size_t i = 0; i < example.rules.size(); ++i) {
            sameScope(metadata.scopeRules[i].folder.Get(), example.rules[i].folder.Get());
            require(metadata.scopeRules[i].recursive == example.rules[i].recursive && metadata.scopeRules[i].excluded == example.rules[i].excluded,
                    "protective import rewrote original recursion/exclusion flags");
        }
        require(metadata.query.find(L"System.ItemFolderPathDisplay") == std::wstring::npos &&
                metadata.query.find(L"System.ItemPathDisplay") == std::wstring::npos,
                "protective importer exposed its derived domain guard as user query text");
        checkedResults([&] { return results(liveRules(metadata.query, metadata.scopeRules).Get()); }, "protected imported query changed actual FileIDs");
        const auto again = fixture.root / (L"protected-again-" + std::to_wstring(index) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(metadata.query, metadata.scopeRules, again, explorer::SearchSaveMode::CreateNew,
                                                   &*metadata.presentation, &*metadata.fileProperties), "re-save exact protected query and metadata");
        checkedResults([&] { return results(saved(again).Get()); }, "protected re-saved native query changed actual FileIDs");
        require(sourceBytes.find("R00UUUUUUUUZDNNU") != std::string::npos && read(again).find("R00UUUUUUUUZDNNU") != std::string::npos,
                "protective import/re-save froze its unresolved Today sibling");
        require(read(source) == sourceBytes && identity(source) == sourceId,
                "protective native/open/import/re-save modified original XML bytes or FileID");
        const auto noGuard = fixture.root / (L"unguarded-" + std::to_wstring(index) + L".search-ms");
        const auto noGuardBytes = replacePayload(sourceBytes, unguardedQuery); write(noGuard, noGuardBytes);
        const auto noGuardId = identity(noGuard);
        unchangedOnFailure(noGuard, root.Get(), unsupported);
        require(read(noGuard) == noGuardBytes && identity(noGuard) == noGuardId,
                "unguarded unsafe scope read modified its native-only source");
        auto nativeOnly = saved(noGuard); SFGAOF flags = 0;
        succeeded(nativeOnly->GetAttributes(SFGAO_FOLDER, &flags), "read native-only unguarded scope attributes");
        require((flags & SFGAO_FOLDER) != 0, "unguarded unsafe scopes stopped opening through the native viewer");
        if (index == 1) {
            std::vector<std::string> altered;
            auto wrongOperation = sourceBytes;
            const auto folderProperty = wrongOperation.find("property=\"System.ItemFolderPathDisplay\"");
            const auto operation = wrongOperation.find("operator=\"eq\"", folderProperty);
            require(folderProperty != std::string::npos && operation != std::string::npos, "native protective operation marker absent");
            wrongOperation.replace(operation, std::string("operator=\"eq\"").size(), "operator=\"neq\"");
            altered.push_back(std::move(wrongOperation));
            auto missingMeaning = sourceBytes;
            const auto meaning = missingMeaning.find(" valuetype=\"System.StructuredQueryType.String\"", folderProperty);
            require(meaning != std::string::npos, "native protective semantic marker absent");
            missingMeaning.erase(meaning, std::string(" valuetype=\"System.StructuredQueryType.String\"").size());
            altered.push_back(std::move(missingMeaning));
            auto wrongProperty = sourceBytes;
            const auto self = wrongProperty.find("property=\"System.ItemPathDisplay\"");
            require(self != std::string::npos, "native protective folder-self marker absent");
            wrongProperty.replace(self, std::string("property=\"System.ItemPathDisplay\"").size(), "property=\"System.ItemNameDisplay\"");
            altered.push_back(std::move(wrongProperty));
            auto outerOr = sourceBytes;
            const auto outer = outerOr.find("<condition type=\"andCondition\">", outerOr.find("<conditions>"));
            require(outer != std::string::npos, "native protective outer AND marker absent");
            outerOr.replace(outer, std::string("<condition type=\"andCondition\">").size(), "<condition type=\"orCondition\">");
            altered.push_back(std::move(outerOr));
            auto wrongRules = sourceBytes;
            const auto included = wrongRules.find("<include"), recursion = wrongRules.find("nonRecursive=\"false\"", included);
            require(included != std::string::npos && recursion != std::string::npos, "protective included recursion marker absent");
            wrongRules.replace(recursion, std::string("nonRecursive=\"false\"").size(), "nonRecursive=\"true\"");
            altered.push_back(std::move(wrongRules));
            unsigned rejectedIndex = 0;
            for (const auto& bytes : altered) {
                const auto rejected = fixture.root / (L"altered-guard-" + std::to_wstring(++rejectedIndex) + L".search-ms");
                write(rejected, bytes); const auto rejectedId = identity(rejected);
                unchangedOnFailure(rejected, root.Get(), unsupported);
                require(read(rejected) == bytes && identity(rejected) == rejectedId,
                        "rejected partial/different protective guard changed XML bytes or FileID");
            }
        }
    }
    for (const auto& [path, id] : originalMembers) require(identity(path) == id, "protective scope fixture changed original member FileID");
    for (const auto& path : {direct, child, deep, ignored, peer, outside, historic})
        require(read(path) == "unchanged protected scope member", "protective scope fixture changed source contents");
}

void publicBooleanLeafParity() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"Boolean scope";
    const auto folderPath = scopePath / L"target-folder";
    require(fs::create_directories(folderPath), "create owned Boolean folder");
    const auto filePath = scopePath / L"target-file.txt";
    write(filePath, "unchanged owned Boolean file");
    const auto outsideFile = fixture.root / L"outside-file.txt";
    const auto outsideFolder = fixture.root / L"outside-folder";
    write(outsideFile, "unchanged outside Boolean control");
    require(fs::create_directory(outsideFolder), "create outside Boolean folder control");
    const auto fileId = identity(filePath), folderId = identity(folderPath);
    const auto outsideFileId = identity(outsideFile), outsideFolderId = identity(outsideFolder);
    auto scope = shellItem(scopePath);
    ComPtr<IPropertyDescription> description;
    succeeded(PSGetPropertyDescriptionByName(L"System.IsFolder", IID_PPV_ARGS(&description)),
              "resolve installed public Boolean property schema");
    VARTYPE propertyType = VT_EMPTY;
    succeeded(description->GetPropertyType(&propertyType), "read installed Boolean property type");
    require(propertyType == VT_BOOL, "installed System.IsFolder is not scalar Boolean");
    PROPERTYKEY key{};
    succeeded(description->GetPropertyKey(&key), "read installed Boolean property key");
    ComPtr<IConditionFactory2> conditionFactory;
    succeeded(CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&conditionFactory)), "create independent native Boolean factory");
    ComPtr<IShellItemArray> scopeArray;
    succeeded(SHCreateShellItemArrayFromShellItem(scope.Get(), IID_PPV_ARGS(&scopeArray)),
              "create exclusively owned native Boolean scope array");
    const auto typedSearch = [&](bool value) {
        ComPtr<ICondition> leaf;
        succeeded(conditionFactory->CreateBooleanLeaf(key, COP_EQUAL, value ? TRUE : FALSE,
                    CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&leaf)), "create independent typed Boolean leaf");
        ComPtr<ISearchFolderItemFactory> searchFactory;
        succeeded(CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&searchFactory)), "create independent Boolean search factory");
        succeeded(searchFactory->SetScope(scopeArray.Get()), "set exclusively owned Boolean search scope");
        succeeded(searchFactory->SetCondition(leaf.Get()), "set typed Boolean search condition");
        ComPtr<IShellItem> search;
        succeeded(searchFactory->GetShellItem(IID_PPV_ARGS(&search)), "create independent typed Boolean search");
        return search;
    };
    const auto base = fixture.root / L"boolean-base.search-ms";
    succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, base), "save public Boolean XML scaffold");
    const auto scaffold = read(base);
    const auto start = scaffold.find("<conditions>"), finish = scaffold.find("</conditions>");
    require(start != std::string::npos && finish != std::string::npos && finish > start,
            "public Boolean scaffold condition boundary missing");
    const auto xmlFor = [&](const std::string& leaf) {
        auto bytes = scaffold;
        bytes.replace(start + std::string("<conditions>").size(),
                      finish - start - std::string("<conditions>").size(), leaf);
        return bytes;
    };
    struct Example { const char* literal; bool value; };
    constexpr std::array examples{Example{"TRUE", true}, Example{"FALSE", false}, Example{"true", true}};
    unsigned index = 0;
    for (const auto& example : examples) {
        const std::set<Identity> expected{example.value ? folderId : fileId};
        eventuallyResults([&] { return results(typedSearch(example.value).Get()); }, expected,
                          "independent typed Boolean search has wrong native FileIDs");
        const auto source = fixture.root / (L"public-boolean-" + std::to_wstring(++index) + L".search-ms");
        // The documented public example omits propertyType. The installed
        // schema, not the literal or an assumed property name, supplies VT_BOOL.
        const auto bytes = xmlFor(std::string("<condition type=\"leafCondition\" property=\"System.IsFolder\" operator=\"eq\" value=\"") +
                                  example.literal + "\"/>");
        write(source, bytes);
        const auto sourceId = identity(source);
        eventuallyResults([&] { return results(saved(source).Get()); }, expected,
                          "public native Boolean XML has wrong FileIDs");
        explorer::SavedSearchMetadata metadata;
        const auto imported = explorer::readSavedSearch(source, &metadata);
        if (FAILED(imported)) {
            require(read(source) == bytes && identity(source) == sourceId,
                    "failed Boolean import modified source XML bytes or FileID");
            std::cerr << "Boolean diagnostic literal=" << example.literal << " importStatus=" <<
                static_cast<unsigned long>(imported) << '\n';
        }
        succeeded(imported, "import schema-proven public Boolean leaf");
        sameScope(metadata.scope.Get(), scope.Get());
        require(metadata.recursive && metadata.scopeRules.size() == 1 && metadata.scopeRules[0].recursive &&
                !metadata.scopeRules[0].excluded && !metadata.query.empty(), "Boolean import changed public query scope metadata");
        sameScope(metadata.scopeRules[0].folder.Get(), scope.Get());
        eventuallyResults([&] { return results(liveRules(metadata.query, metadata.scopeRules).Get()); }, expected,
                          "imported public Boolean query changed native FileIDs");
        const auto resaved = fixture.root / (L"resaved-boolean-" + std::to_wstring(index) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(metadata.query, metadata.scopeRules, resaved),
                  "resave imported public Boolean leaf through unchanged native writer");
        eventuallyResults([&] { return results(saved(resaved).Get()); }, expected,
                          "resaved Boolean XML changed native FileIDs");
        explorer::SavedSearchMetadata restored;
        succeeded(explorer::readSavedSearch(resaved, &restored), "reimport native resaved Boolean query");
        eventuallyResults([&] { return results(liveRules(restored.query, restored.scopeRules).Get()); }, expected,
                          "reimported Boolean query changed native FileIDs");
        require(read(source) == bytes && identity(source) == sourceId,
                "Boolean native open/import/re-save modified source XML bytes or FileID");
    }
    const std::array rejected{
        "<condition type=\"leafCondition\" property=\"System.IsFolder\" operator=\"eq\" value=\"maybe\"/>",
        "<condition type=\"leafCondition\" property=\"System.IsFolder\" operator=\"eq\" value=\"1\"/>",
        "<condition type=\"leafCondition\" property=\"System.IsFolder\" operator=\"eq\" value=\"\"/>",
        "<condition type=\"leafCondition\" property=\"System.FileName\" operator=\"eq\" value=\"TRUE\"/>",
        "<condition type=\"leafCondition\" property=\"WindowsExplorer.OwnedUnknownBoolean\" operator=\"eq\" value=\"TRUE\"/>",
        "<condition type=\"leafCondition\" property=\"System.IsFolder\" operator=\"eq\" value=\"TRUE\" valuetype=\"System.StructuredQueryType.String\"/>",
        "<condition type=\"leafCondition\" property=\"System.IsFolder\" operator=\"contains\" value=\"TRUE\"/>",
        "<condition type=\"leafCondition\" property=\"System.IsFolder\" propertyType=\"integer\" operator=\"eq\" value=\"TRUE\"/>"};
    for (const auto* leaf : rejected) {
        const auto source = fixture.root / (L"rejected-boolean-" + std::to_wstring(++index) + L".search-ms");
        const auto bytes = xmlFor(leaf); write(source, bytes);
        const auto sourceId = identity(source);
        unchangedOnFailure(source, scope.Get());
        require(read(source) == bytes && identity(source) == sourceId,
                "rejected Boolean import modified source XML or FileID");
    }
    require(identity(filePath) == fileId && read(filePath) == "unchanged owned Boolean file" &&
            identity(folderPath) == folderId && fs::is_empty(folderPath) &&
            identity(outsideFile) == outsideFileId && read(outsideFile) == "unchanged outside Boolean control" &&
            identity(outsideFolder) == outsideFolderId && fs::is_empty(outsideFolder),
            "Boolean verification modified its owned members or outside-scope controls");
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
    changed("</persistedQuery>", "<properties><description><unknown/></description></properties></persistedQuery>", 15);
    changed("</persistedQuery>", "<properties><unknown/></properties></persistedQuery>", 16);
    changed("</persistedQuery>", "<properties xmlns=\"urn:unsupported\"/></persistedQuery>", 17);
    changed("</persistedQuery>", "<properties>owned metadata sentinel</properties></persistedQuery>", 18);
    changed("</persistedQuery>", "<properties><author/><author/></properties></persistedQuery>", 19);
    changed("</persistedQuery>", "<properties><tags unknown=\"attribute\">owned</tags></properties></persistedQuery>", 20);
    changed("</persistedQuery>", "<properties><description>" + std::string(32769, 'x') + "</description></properties></persistedQuery>", 21);
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
    // External unresolved Blurb is not self-describing. The writer resolves
    // parser-produced Blurb through its original solution before publication;
    // an imported file has no such context and must still preserve the output
    // sentinel while rejecting that unsupported type.
    changed("valuetype=\"System.StructuredQueryType.Integer\"",
            "valuetype=\"System.StructuredQueryType.Blurb\"", 22);
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
void filePropertiesRoundTrip() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"owned scope";
    require(fs::create_directory(scopePath), "create only owned metadata property scope");
    const auto first = scopePath / L"first.txt", second = scopePath / L"second.bin";
    write(first, "unchanged first"); write(second, "unchanged second");
    const auto firstId = identity(first), secondId = identity(second);
    const std::set<Identity> expectedMembers{firstId, secondId};
    auto scope = shellItem(scopePath);
    const auto source = fixture.root / L"external-properties.search-ms";
    succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, source), "create valid owned external metadata base");
    auto bytes = read(source);
    const auto end = bytes.find("</persistedQuery>"); require(end != std::string::npos, "find owned property insertion marker");
    bytes.insert(end, utf8(L"<properties><author>  作者 &amp; &lt; &gt; &quot; &apos; 🚀&#9;&#13;&#10;  </author>"
        L"<kind>searchfolder</kind><description><![CDATA[ first line ]]>&#13;&#10;second &amp; end</description>"
        L"<tags>one;two;資料</tags></properties>\r\n"));
    write(source, bytes);
    explorer::SearchFileProperties expected;
    expected.author = L"  作者 & < > \" ' 🚀\t\r\n  "; expected.kind = L"searchfolder";
    expected.description = L" first line \r\nsecond & end"; expected.tags = L"one;two;資料";
    explorer::SavedSearchMetadata imported;
    succeeded(explorer::readSavedSearch(source, &imported), "import supported owned file property text");
    require(imported.fileProperties && *imported.fileProperties == expected, "import changed exact file property text/whitespace/Unicode");
    require(results(saved(source).Get()) == expectedMembers && results(liveRules(imported.query, imported.scopeRules).Get()) == expectedMembers,
        "file properties changed native query membership");
    const auto output = fixture.root / L"refined-properties.search-ms";
    succeeded(explorer::saveSearchForScopeRules(imported.query + L" AND System.Size:>=1", imported.scopeRules, output,
        explorer::SearchSaveMode::CreateNew, nullptr, &*imported.fileProperties), "refine/re-save imported owned file properties");
    explorer::SavedSearchMetadata restored;
    succeeded(explorer::readSavedSearch(output, &restored), "re-import refined file properties");
    require(restored.fileProperties && *restored.fileProperties == expected, "refined file property round trip changed literal values");
    require(results(saved(output).Get()) == expectedMembers, "refined file property round trip changed native saved identities");
    require(results(liveRules(restored.query, restored.scopeRules).Get()) == expectedMembers, "refined file property round trip changed recreated live identities");
    const auto before = read(output);
    const auto beforeId = identity(output);
    for (const auto& invalid : {std::wstring(32769, L'x'), std::wstring(L"x\0y", 3),
        std::wstring(1, static_cast<wchar_t>(0xD800)), std::wstring(1, static_cast<wchar_t>(0xFFFF)), std::wstring(1, L'\1')}) {
        auto properties = expected; properties.description = invalid;
        require(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, output, explorer::SearchSaveMode::UserConfirmed, nullptr, &properties) == E_INVALIDARG &&
            read(output) == before && identity(output) == beforeId, "invalid file property replaced an existing owned saved search");
    }
    explorer::SearchFileProperties empty;
    const auto emptyPath = fixture.root / L"empty-properties.search-ms";
    succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, emptyPath, explorer::SearchSaveMode::CreateNew, nullptr, &empty),
        "preserve an empty optional properties element");
    succeeded(explorer::readSavedSearch(emptyPath, &restored), "import empty optional properties");
    require(restored.fileProperties && *restored.fileProperties == empty, "empty file metadata became missing or populated");
    expected.author = std::wstring(32768, L'&');
    const auto boundary = fixture.root / L"bounded-properties.search-ms";
    succeeded(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, boundary, explorer::SearchSaveMode::CreateNew, nullptr, &expected),
        "save maximum bounded escaped file property");
    succeeded(explorer::readSavedSearch(boundary, &restored), "import maximum bounded escaped file property");
    require(restored.fileProperties && *restored.fileProperties == expected, "bounded escaped property lost data");
    require(read(source) == bytes && identity(first) == firstId && identity(second) == secondId &&
        read(first) == "unchanged first" && read(second) == "unchanged second", "property preservation mutated original query or owned sources");
}
void serializedReaderLimitPreflight() {
    Fixture fixture; auto scope = shellItem(fixture.root);
    const auto query = [](unsigned count) {
        std::wstring value;
        for (unsigned index = 1; index <= count; ++index) {
            if (!value.empty()) value += L' ';
            value += L"a" + std::to_wstring(index);
        }
        return value;
    };
    const auto accepted = fixture.root / L"bounded-generated.search-ms";
    succeeded(explorer::saveSearch(query(1800), scope.Get(), true, accepted), "save large importable canonical native condition tree");
    explorer::SavedSearchMetadata imported;
    succeeded(explorer::readSavedSearch(accepted, &imported), "re-import large generated native condition tree within DOM limits");
    const auto before = read(accepted); const auto beforeId = identity(accepted);
    const std::vector<explorer::SearchScopeRule> scopes(256, {scope, true, false});
    const auto oversized = query(1800);
    require(oversized.size() <= explorer::LiveSearchPolicy::maximumLiteralLength, "node-limit fixture must be a valid bounded literal");
    const auto fresh = fixture.root / L"unimportable-generated.search-ms";
    const auto rejected = explorer::saveSearchForScopeRules(oversized, scopes, fresh);
    if (rejected != unsupported) {
        std::ostringstream diagnostic;
        diagnostic << "generated DOM-node-budget tree returned HRESULT 0x" << std::hex << static_cast<unsigned long>(rejected)
            << " instead of ERROR_NOT_SUPPORTED";
        throw std::runtime_error(diagnostic.str());
    }
    require(!fs::exists(fresh), "writer published an unreadable DOM-node-budget search");
    require(explorer::saveSearchForScopeRules(oversized, scopes, accepted, explorer::SearchSaveMode::UserConfirmed) == unsupported &&
        read(accepted) == before && identity(accepted) == beforeId,
        "DOM-limit preflight changed an existing saved query before rejecting its replacement");
    require(std::distance(fs::directory_iterator(fixture.root), fs::directory_iterator{}) == 1,
        "DOM-limit rejection left a staging file or extra generated query");
}
void longUnicodeNativePathsAndTypedTargets() {
    Fixture fixture;
    auto scopePath = fixture.root;
    for (unsigned index = 0; index < 6; ++index)
        scopePath /= L"Owned long 日本語 λ 🚀 segment " + std::to_wstring(index) + std::wstring(24, L'x');
    const auto nestedPath = scopePath / L"Nested 資料 🚀";
    require(scopePath.native().size() > MAX_PATH + 80 && fs::create_directories(extendedPath(nestedPath)),
        "create exclusively owned real extended Unicode hierarchy beyond MAX_PATH");
    const auto direct = scopePath / L"match-日本語 🚀.txt", nested = nestedPath / L"match-nested λ.txt";
    const auto outside = fixture.root / L"match-outside.txt", tool = scopePath / L"Owned Unicode ツール 🚀.exe";
    write(direct, "unchanged owned long direct"); write(nested, "unchanged owned long nested");
    write(outside, "unchanged owned outside"); write(tool, "owned inert bytes, never executed");
    const auto directId = identity(direct), nestedId = identity(nested), outsideId = identity(outside), toolId = identity(tool);
    auto scope = shellItem(extendedPath(scopePath));
    const std::wstring query = L"System.FileName:~<\"match-\"";
    const auto members = [](IShellItem* item, const char* stage) {
        try { return results(item); }
        catch (const std::exception& error) { throw std::runtime_error(std::string(stage) + ": " + error.what()); }
    };
    for (const bool recursive : {false, true}) {
        const std::set<Identity> expected = recursive ? std::set<Identity>{directId, nestedId} : std::set<Identity>{directId};
        require(members(live(query, scope.Get(), recursive).Get(), "long live scope") == expected, "long Unicode live scope changed native FileIDs or escaped scope");
        const auto output = scopePath.parent_path() / (recursive ? L"Long recursive.search-ms" : L"Long shallow.search-ms");
        require(output.native().size() > MAX_PATH, "saved destination must also exceed MAX_PATH");
        succeeded(explorer::saveSearch(query, scope.Get(), recursive, extendedPath(output)), "save actual long Unicode native scope/destination");
        const auto outputBytes = read(output); const auto outputId = identity(output);
        // Windows 10 accepts the long filesystem item but its saved-search
        // loader cannot bind a .search-ms filename beyond MAX_PATH. Preserve
        // that real native failure, and prove the exact bytes/scopes remain a
        // valid native query at another exclusively owned, short destination.
        ComPtr<IShellFolder> longFolder;
        require(saved(extendedPath(output))->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&longFolder)) ==
            HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && !longFolder,
            "native long saved-search destination did not report its exact buffer-limit failure");
        const auto nativeOutput = fixture.root / (recursive ? L"Native long recursive.search-ms" : L"Native long shallow.search-ms");
        write(nativeOutput, outputBytes);
        require(members(saved(nativeOutput).Get(), "native loader with long saved scope") == expected,
            "identical long-scope saved bytes changed exact native member identities");
        explorer::SavedSearchMetadata imported;
        succeeded(explorer::readSavedSearch(extendedPath(output), &imported), "import actual long Unicode saved scope");
        sameScope(imported.scope.Get(), scope.Get());
        require(imported.recursive == recursive && members(liveRules(imported.query, imported.scopeRules).Get(), "long imported live scope") == expected,
            "long saved scope import changed canonical identity, recursion or membership");
        const auto refined = scopePath.parent_path() / (recursive ? L"Long recursive refined.search-ms" : L"Long shallow refined.search-ms");
        succeeded(explorer::saveSearchForScopeRules(imported.query + L" AND System.Size:>0", imported.scopeRules, extendedPath(refined)),
            "refine/re-save actual long native scope");
        const auto nativeRefined = fixture.root / (recursive ? L"Native long recursive refined.search-ms" : L"Native long shallow refined.search-ms");
        write(nativeRefined, read(refined));
        require(members(saved(nativeRefined).Get(), "native loader with refined long saved scope") == expected,
            "identical refined long-scope saved bytes changed exact native members");
        require(read(output) == outputBytes && identity(output) == outputId,
            "native destination failure or metadata refinement changed the original long saved file");
    }
    const auto extendedTool = extendedPath(tool).native();
    const std::wstring parameters = L"/owned \"資料 λ 🚀\" /reference:other.exe";
    for (const auto& input : {L"\"" + extendedTool + L"\" " + parameters, extendedTool + L" " + parameters}) {
        explorer::TypedAddressLaunch parsed;
        succeeded(explorer::parseTypedAddressLaunch(input, &parsed), "parse actual long Unicode extended target and exact parameters");
        require(parsed.target == extendedTool && parsed.parameters == parameters, "long typed address lost target or literal argument spelling");
        auto target = shellItem(parsed.target);
        require(identity(filesystemPath(target.Get())) == toolId, "typed long target did not resolve to its actual owned native FileID");
        require(explorer::launchTypedAddress(nullptr, input, true, {}, extendedTool) == E_ACCESSDENIED,
            "long typed target escaped the headless launch prohibition");
    }
    require(identity(direct) == directId && identity(nested) == nestedId && identity(outside) == outsideId && identity(tool) == toolId &&
        read(direct) == "unchanged owned long direct" && read(nested) == "unchanged owned long nested" &&
        read(outside) == "unchanged owned outside" && read(tool) == "owned inert bytes, never executed",
        "long native path/address search verification changed owned sources");
}
void namedBlurbAndFilenameWordPrefixParity() {
    Fixture fixture;
    const auto scopePath=fixture.root/L"Owned prefix 資料 日本語";
    require(fs::create_directories(scopePath/L"nested"),"create native filename prefix hierarchy");
    require(fs::create_directory(fixture.root/L"outside"),"create prefix outside sentinel");
    constexpr std::array names{L"match.txt",L"matching.txt",L"before match.txt",L"before matching.txt",
        L"prematch.txt",L"資料 match 文件.txt",L"match extra.txt",L"matchmore extraneous.txt",
        L"match more extra.txt",L"rematch extraneous.txt",L"other.txt",L"extra match.txt",L"extraneous matchmore.txt"};
    for(const auto* name:names)write(scopePath/name,"unchanged owned prefix source");
    const auto nested=scopePath/L"nested"/L"matching.txt";
    write(nested,"unchanged owned nested prefix source");
    const auto outside=fixture.root/L"outside"/L"matching.txt";
    write(outside,"unchanged outside prefix sentinel");
    const auto outsideId=identity(outside);
    auto scope=shellItem(scopePath);
    std::set<Identity> prefix,allFiles;
    for(size_t index=0;index<names.size();++index) {
        allFiles.insert(identity(scopePath/names[index]));
        if(index!=4&&index!=9&&index!=10)prefix.insert(identity(scopePath/names[index]));
    }
    const auto shallow=prefix;
    prefix.insert(identity(nested));allFiles.insert(identity(nested));
    // The installed unindexed filename provider matches all requested word
    // prefixes, including separated/reversed words. Preserve those actual
    // native identities across XML/import rather than assuming phrase order.
    const std::set<Identity> phrase{identity(scopePath/L"match extra.txt"),identity(scopePath/L"matchmore extraneous.txt"),
        identity(scopePath/L"match more extra.txt"),identity(scopePath/L"extra match.txt"),identity(scopePath/L"extraneous matchmore.txt")};
    auto either=prefix;either.insert(identity(scopePath/L"other.txt"));
    const std::set<Identity> negated{identity(scopePath/L"prematch.txt"),identity(scopePath/L"rematch extraneous.txt"),identity(scopePath/L"other.txt")};
    auto bareNegated=negated;bareNegated.insert(identity(scopePath/L"nested"));
    struct Case {const wchar_t* name;const wchar_t* query;bool recursive;std::set<Identity> expected;};
    const std::array cases{
        Case{L"single",L"System.FileName:$<\"match\"",true,prefix},
        Case{L"multiword",L"System.FileName:$<\"match extra\"",true,phrase},
        Case{L"and",L"System.Size:>0 AND System.FileName:$<\"match\"",true,prefix},
        Case{L"or",L"System.FileName:$<\"match\" OR System.FileName:=\"other.txt\"",true,either},
        Case{L"not",L"System.FileExtension:=\".txt\" AND NOT System.FileName:$<\"match\"",true,negated},
        Case{L"shallow",L"System.FileName:$<\"match\"",false,shallow},
        Case{L"extension",L"System.FileExtension:=\".txt\"",true,allFiles},
        Case{L"bare-not",L"NOT System.FileName:$<\"match\"",true,bareNegated}};
    for(const auto& test:cases) {
        const auto source=fixture.root/(std::wstring(test.name)+L".search-ms");
        require(results(live(test.query,scope.Get(),test.recursive).Get())==test.expected,
            "live filename prefix/named-string condition changed actual owned FileIDs");
        succeeded(explorer::saveSearch(test.query,scope.Get(),test.recursive,source),"save native filename prefix/named-string condition");
        const auto bytes=read(source);
        require(bytes.find("System.StructuredQueryType.Blurb")==std::string::npos,
            "saved named string leaked unresolved parser-dependent Blurb token");
        if(std::wstring_view(test.query).find(L"$<")!=std::wstring_view::npos)
            require(bytes.find("operator=\"wordmatch\"")!=std::string::npos,"saved word-prefix changed operator");
        require(results(saved(source).Get())==test.expected,"native saved word-prefix/named-string result IDs differ");
        explorer::SavedSearchMetadata imported;
        succeeded(explorer::readSavedSearch(source,&imported),"import supported word-prefix/named-string metadata");
        sameScope(imported.scope.Get(),scope.Get());
        require(imported.recursive==test.recursive&&results(liveRules(imported.query,imported.scopeRules).Get())==test.expected,
            "imported word-prefix/named-string condition lost scope or native FileIDs");
        const auto second=fixture.root/(std::wstring(test.name)+L"-roundtrip.search-ms");
        succeeded(explorer::saveSearchForScopeRules(imported.query,imported.scopeRules,second),"resave native prefix/named-string metadata");
        require(results(saved(second).Get())==test.expected,"second native word-prefix/named-string save changed FileIDs");
        explorer::SavedSearchMetadata restored;
        succeeded(explorer::readSavedSearch(second,&restored),"reimport second native prefix/named-string save");
        require(results(liveRules(restored.query,restored.scopeRules).Get())==test.expected,
            "second prefix/named-string import changed native FileIDs");
    }
    for(const auto* name:names)require(allFiles.contains(identity(scopePath/name))&&read(scopePath/name)=="unchanged owned prefix source",
        "native prefix verification modified its owned source");
    require(identity(outside)==outsideId&&read(outside)=="unchanged outside prefix sentinel",
        "native prefix verification changed outside sentinel");
}
} // namespace

int runSavedSearchTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"saved metadata native query/scope/result parity", metadataParity},
        {"generic prefix/phrase/numeric/Boolean/date native saved identity parity", genericQueryParity},
        {"named Blurb and filename word-prefix live/native/import/re-save exact FileIDs", namedBlurbAndFilenameWordPrefixParity},
        {"owned native Library live/saved/restored scope identity parity", ownedLibraryScopeParity},
        {"literal percent native results and virtual known-folder metadata identity", literalPercentAndKnownFolderScopes},
        {"native include/exclude/mixed recursion rules and lossless re-save identities", includeExcludeAndMixedScopeRules},
        {"guarded shallow/equal-root/direct-child physical scopes four-route native FileIDs", protectedPhysicalScopeParity},
        {"native kind union imported/restated/re-saved exact identity parity", nativeKindUnionMetadataParity},
        {"saved metadata shallow/recursive exact fixture identities", recursiveAndShallow},
        {"saved metadata unresolved relative dates and This PC", relativeDateAndThisPc},
        {"public Boolean leaves without propertyType native import/re-save exact FileIDs", publicBooleanLeafParity},
        {"saved metadata unsupported external shapes and output preservation", unsupportedExternalShapes},
        {"saved metadata malformed/oversize/deep/node limits and DTD prohibition", malformedBoundsAndDtd},
        {"saved file properties native import/refine/re-save text and identity preservation", filePropertiesRoundTrip},
        {"generated saved query reader-limit preflight and existing-file preservation", serializedReaderLimitPreflight},
        {"native long Unicode scopes/typed FileIDs and exact saved-loader path limit", longUnicodeNativePathsAndTypedTargets}
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
