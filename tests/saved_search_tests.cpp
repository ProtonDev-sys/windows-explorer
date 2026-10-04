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
struct Fixture {
    fs::path root;
    Fixture() {
        GUID id{}; succeeded(CoCreateGuid(&id), "create fixture identifier");
        wchar_t text[40]{}; require(StringFromGUID2(id, text, 40) != 0, "format identifier");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-saved-metadata-資料&-") + text);
        require(fs::create_directory(root), "create fixture directory");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
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
        require(found.insert(identity(filesystemPath(item.Get()))).second, "duplicate native result identity");
    }
    return found;
}
ComPtr<IShellItem> live(const std::wstring& query, IShellItem* scope, bool recursive = true) {
    ComPtr<IShellItem> item; succeeded(explorer::createSearchFolder(query, scope, &item, recursive), "create restored live query"); return item;
}
ComPtr<IShellItem> saved(const fs::path& path) { return shellItem(path); }
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
void unchangedOnFailure(const fs::path& file, IShellItem* sentinel, HRESULT expected = S_OK) {
    explorer::SavedSearchMetadata output{L"unchanged query", sentinel, false};
    const auto hr = explorer::readSavedSearch(file, &output);
    require(FAILED(hr), "unsupported/malformed saved metadata accepted");
    if (expected != S_OK && hr != expected)
        throw std::runtime_error("unexpected metadata rejection result for " + utf8(file.filename().native()) + ": " + std::to_string(static_cast<unsigned long>(hr)));
    require(output.query == L"unchanged query" && !output.recursive && output.scope.Get() == sentinel, "failed metadata read changed caller output");
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
    changed("</scope>", "<include path=\"C:\\\"/></scope>", 1);
    changed("</scope>", "<exclude path=\"C:\\\"/></scope>", 2);
    changed("<query>", "<query><providers/>", 3);
    changed("<query>", "<query><subQueries/>", 4);
    changed("name=\"item\"", "name=\"document\"", 5);
    changed("propertyType=\"wstr\"", "propertyType=\"integer\"", 6);
    changed("operator=\"gt\"", "operator=\"wordstarts with\"", 7);
    changed("<conditions>", "<conditions><attributes/>", 8);
    changed("persistedQuery version=\"1.0\"", "persistedQuery version=\"2.0\"", 9);
    changed("<query>", "<query xmlns=\"urn:unsupported\">", 10);
    changed("</kindList>", "<kind name=\"item\"/></kindList>", 11);
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
    const auto rejected = fixture.root / L"generic.search-ms";
    for (const auto* query : {L"target", L"\"target\"", L"target AND System.Size:>1", L"target OR System.FileName:=\"other.txt\""}) {
        require(explorer::saveSearch(query, scope.Get(), true, rejected) == unsupported, "unspecified-property persistence accepted without verified semantics");
        require(!fs::exists(rejected), "unsupported generic persistence created a file");
    }
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
