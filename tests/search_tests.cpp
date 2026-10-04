#include "explorer/search.hpp"

#include <shlobj.h>
#include <shlguid.h>
#include <propkey.h>
#include <msxml6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <compare>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << ")\n";
        throw std::runtime_error(message);
    }
}
void write(const fs::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(stream.good(), "write search fixture");
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "read saved search");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct Fixture {
    fs::path root;
    bool shortAlias = false;
    Fixture() {
        GUID guid{};
        succeeded(CoCreateGuid(&guid), "create search fixture id");
        wchar_t identifier[40]{};
        require(StringFromGUID2(guid, identifier, 40) != 0, "format search fixture id");
        root = fs::temp_directory_path() / (std::wstring(L"windows-explorer-search-test-資料&-") + identifier);
        require(fs::create_directory(root), "create exclusive search fixture");
        wchar_t aliasMode[2]{};
        if (GetEnvironmentVariableW(L"WINDOWSEXPLORER_SEARCH_TEST_SHORT_PATHS", aliasMode, 2) == 1 && aliasMode[0] == L'1') {
            const auto length = GetShortPathNameW(root.c_str(), nullptr, 0);
            require(length != 0, "get deliberate fixture short alias length");
            std::vector<wchar_t> alias(length);
            require(GetShortPathNameW(root.c_str(), alias.data(), length) != 0, "get deliberate fixture short alias");
            require(_wcsicmp(root.c_str(), alias.data()) != 0, "deliberate fixture short alias unavailable");
            root = alias.data();
            shortAlias = true;
        }
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};
ComPtr<IShellItem> shellItem(const fs::path& path) {
    ComPtr<IShellItem> result;
    succeeded(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "parse search fixture item");
    return result;
}

std::wstring nativeFilesystemPath(IShellItem* item) {
    PWSTR raw = nullptr;
    const auto hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    std::wstring path = raw ? raw : L"";
    CoTaskMemFree(raw);
    succeeded(hr, "read canonical native fixture path");
    require(!path.empty(), "canonical native fixture path is empty");
    return path;
}
std::string utf8(const std::wstring& text) {
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(length > 0 || text.empty(), "encode fixture diagnostic");
    std::string result(static_cast<size_t>(length), '\0');
    if (length) require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr) != 0,
                        "encode fixture diagnostic bytes");
    return result;
}
struct FileIdentity {
    ULONGLONG volume = 0;
    std::array<BYTE, 16> identifier{};
    auto operator<=>(const FileIdentity&) const = default;
};
FileIdentity fileIdentity(const std::wstring& path) {
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        std::cerr << "cannot open result identity: " << utf8(path) << '\n';
        succeeded(HRESULT_FROM_WIN32(error), "open exact fixture file identity");
    }
    FILE_ID_INFO info{};
    const BOOL readInfo = GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info));
    const auto hr = readInfo ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(handle);
    succeeded(hr, "read volume and 128-bit fixture file identity");
    FileIdentity result{info.VolumeSerialNumber};
    std::copy_n(info.FileId.Identifier, result.identifier.size(), result.identifier.begin());
    return result;
}
void requireResults(const std::set<std::wstring>& actual, const std::set<std::wstring>& expected,
                    const char* message) {
    std::set<FileIdentity> actualIds, expectedIds;
    for (const auto& path : actual) actualIds.insert(fileIdentity(path));
    for (const auto& path : expected) expectedIds.insert(fileIdentity(path));
    // Do not weaken membership to equal counts or basename matching. Native
    // Shell names may expand an 8.3 alias or change casing for the same file.
    const bool matches = actual.size() == expected.size() && actualIds == expectedIds &&
                         actualIds.size() == actual.size() && expectedIds.size() == expected.size();
    if (!matches) {
        std::cerr << message << "\nactual (" << actual.size() << "):\n";
        for (const auto& path : actual) std::cerr << "  " << utf8(path) << '\n';
        std::cerr << "expected (" << expected.size() << "):\n";
        for (const auto& path : expected) std::cerr << "  " << utf8(path) << '\n';
        std::cerr << "distinct file identities: actual=" << actualIds.size()
                  << ", expected=" << expectedIds.size() << '\n';
    }
    require(matches, message);
}

struct XmlString {
    BSTR value;
    explicit XmlString(const wchar_t* text) : value(SysAllocString(text)) {
        require(value != nullptr, "allocate XML string");
    }
    ~XmlString() { SysFreeString(value); }
};
struct AutomationVariant {
    VARIANT value{};
    ~AutomationVariant() { VariantClear(&value); }
};
ComPtr<IXMLDOMDocument2> loadXml(const fs::path& path) {
    ComPtr<IXMLDOMDocument2> document;
    succeeded(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&document)), "create headless native XML parser");
    succeeded(document->put_async(VARIANT_FALSE), "make XML parsing synchronous");
    succeeded(document->put_resolveExternals(VARIANT_FALSE), "disable external XML resolution");
    succeeded(document->put_validateOnParse(VARIANT_FALSE), "disable external XML validation");
    XmlString prohibit(L"ProhibitDTD");
    AutomationVariant enabled;
    enabled.value.vt = VT_BOOL; enabled.value.boolVal = VARIANT_TRUE;
    succeeded(document->setProperty(prohibit.value, enabled.value), "prohibit XML DTD");
    AutomationVariant source;
    source.value.vt = VT_BSTR; source.value.bstrVal = SysAllocString(path.c_str());
    require(source.value.bstrVal != nullptr, "allocate XML source");
    VARIANT_BOOL loaded = VARIANT_FALSE;
    succeeded(document->load(source.value, &loaded), "load saved search XML");
    require(loaded == VARIANT_TRUE, "saved search is not valid XML");
    return document;
}
ComPtr<IXMLDOMElement> xmlElement(IXMLDOMDocument2* document, const wchar_t* xpath) {
    XmlString expression(xpath);
    ComPtr<IXMLDOMNode> node;
    succeeded(document->selectSingleNode(expression.value, &node), "find saved search element");
    require(node != nullptr, "saved search element missing");
    ComPtr<IXMLDOMElement> element;
    succeeded(node.As(&element), "read saved search element");
    return element;
}
std::wstring xmlAttribute(IXMLDOMElement* element, const wchar_t* name) {
    XmlString attribute(name);
    AutomationVariant value;
    succeeded(element->getAttribute(attribute.value, &value.value), "read saved search attribute");
    require(value.value.vt == VT_BSTR && value.value.bstrVal, "saved search attribute missing");
    return {value.value.bstrVal, SysStringLen(value.value.bstrVal)};
}

// This binds and executes the real native Search Folder without a window,
// Browser host, ShellExecute, global clipboard, or user index modifications.
std::set<std::wstring> searchResults(IShellItem* item) {
    ComPtr<IShellFolder> folder;
    succeeded(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "bind native search folder");
    ComPtr<IEnumIDList> enumerator;
    succeeded(folder->EnumObjects(nullptr, static_cast<SHCONTF>(SHCONTF_FOLDERS | SHCONTF_NONFOLDERS),
                                   &enumerator), "enumerate native fixture search");
    std::set<std::wstring> paths;
    unsigned count = 0;
    while (enumerator) {
        PITEMID_CHILD pidl = nullptr;
        const auto next = enumerator->Next(1, &pidl, nullptr);
        if (next == S_FALSE) break;
        succeeded(next, "read native fixture result");
        require(pidl != nullptr, "native result has no Shell identity");
        ComPtr<IShellItem> result;
        const auto created = SHCreateItemWithParent(nullptr, folder.Get(), pidl, IID_PPV_ARGS(&result));
        CoTaskMemFree(pidl);
        succeeded(created, "resolve native result identity");
        PWSTR path = nullptr;
        const auto named = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
        bool inserted = false;
        if (SUCCEEDED(named) && path) inserted = paths.emplace(path).second;
        CoTaskMemFree(path);
        succeeded(named, "resolve fixture result path");
        require(inserted, "native search returned an empty or duplicate fixture path");
        require(++count <= 256, "fixture search escaped its bounded scope");
    }
    return paths;
}
ComPtr<IShellItem> reopenSearch(const fs::path& path) {
    auto item = shellItem(path);
    SFGAOF attributes = 0;
    succeeded(item->GetAttributes(SFGAO_FOLDER, &attributes), "read saved search Shell attributes");
    require((attributes & SFGAO_FOLDER) != 0, "saved search is not a native Shell folder");
    ComPtr<IShellItem2> metadata;
    succeeded(item.As(&metadata), "read saved search item metadata");
    PWSTR itemType = nullptr;
    const auto typed = metadata->GetString(PKEY_ItemType, &itemType);
    const bool isSearch = SUCCEEDED(typed) && itemType && _wcsicmp(itemType, L".search-ms") == 0;
    CoTaskMemFree(itemType);
    succeeded(typed, "read saved search item type");
    require(isSearch, "saved search lacks native .search-ms item type");
    ComPtr<IShellFolder> folder;
    succeeded(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "reopen saved native query");
    PIDLIST_ABSOLUTE pidl = nullptr;
    succeeded(SHGetIDListFromObject(item.Get(), &pidl), "get saved search Shell identity");
    ComPtr<IShellItem> restored;
    const auto rebuilt = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&restored));
    CoTaskMemFree(pidl);
    succeeded(rebuilt, "rebuild saved search Shell identity");
    int comparison = 1;
    succeeded(item->Compare(restored.Get(), SICHINT_CANONICAL, &comparison), "compare saved search identities");
    require(comparison == 0, "saved search identity changed during PIDL round trip");
    return item;
}

void shallowAndRecursiveResults() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"scope & 資料";
    require(fs::create_directories(scopePath / L"nested"), "create nested search fixture");
    require(fs::create_directory(fixture.root / L"outside"), "create outside search fixture");
    const auto direct = scopePath / L"match & 資料.txt";
    const auto nested = scopePath / L"nested" / L"match & 資料.txt";
    write(direct, "direct match"); write(nested, "nested match");
    write(scopePath / L"different.txt", "not matched");
    write(fixture.root / L"outside" / L"match & 資料.txt", "must never be returned");
    auto scope = shellItem(scopePath);
    if (fixture.shortAlias) {
        const auto canonicalScope = nativeFilesystemPath(scope.Get());
        require(_wcsicmp(scopePath.c_str(), canonicalScope.c_str()) != 0,
                "deliberate fixture alias did not reproduce native path normalization");
        std::cout << "INFO: native search fixture alias normalization reproduced\n"
                  << "  alias: " << utf8(scopePath.native()) << '\n'
                  << "  native: " << utf8(canonicalScope) << '\n';
    }
    const std::wstring query = L"System.FileName:=\"match & 資料.txt\"";
    for (bool recursive : {false, true}) {
        ComPtr<IShellItem> live;
        succeeded(explorer::createSearchFolder(query, scope.Get(), &live, recursive), "create native scoped search");
        const std::set<std::wstring> expected = recursive ?
            std::set<std::wstring>{direct.native(), nested.native()} : std::set<std::wstring>{direct.native()};
        requireResults(searchResults(live.Get()), expected, "native search has wrong recursion or scope");
        const auto savedPath = fixture.root / (recursive ? L"recursive.search-ms" : L"shallow.search-ms");
        succeeded(explorer::saveSearch(query, scope.Get(), recursive, savedPath), "save Unicode native search");
        auto document = loadXml(savedPath);
        const auto include = xmlElement(document.Get(), L"/persistedQuery/query/scope/include");
        const auto savedScope = xmlAttribute(include.Get(), L"path");
        require(savedScope == nativeFilesystemPath(scope.Get()), "XML scope lost canonical Shell path or Unicode/metacharacters");
        require(fileIdentity(savedScope) == fileIdentity(scopePath.native()), "saved XML scope names a different folder");
        require(xmlAttribute(include.Get(), L"nonRecursive") == (recursive ? L"false" : L"true"), "saved scope lost recursion");
        const auto saved = reopenSearch(savedPath);
        requireResults(searchResults(saved.Get()), expected, "reopened native saved search changed its results");
    }
    // A saved query reruns instead of freezing a result snapshot.
    require(fs::create_directories(scopePath / L"new child"), "create new saved-search result scope");
    const auto added = scopePath / L"new child" / L"match & 資料.txt";
    write(added, "created after saving");
    requireResults(searchResults(reopenSearch(fixture.root / L"recursive.search-ms").Get()),
        std::set<std::wstring>{direct.native(), nested.native(), added.native()}, "saved search did not rerun on new fixture content");
    requireResults(searchResults(reopenSearch(fixture.root / L"shallow.search-ms").Get()),
        std::set<std::wstring>{direct.native()}, "saved shallow query included new descendants");
}

void canonicalRefinementsAndRelativeDates() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"scope";
    require(fs::create_directory(scopePath), "create canonical query fixture");
    write(scopePath / L"current.txt", "current document");
    write(scopePath / L"old.txt", "old document");
    const HANDLE oldFile = CreateFileW((scopePath / L"old.txt").c_str(), FILE_WRITE_ATTRIBUTES, 0,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(oldFile != INVALID_HANDLE_VALUE, "open old date fixture");
    const SYSTEMTIME oldDate{2000, 1, 0, 1, 12, 0, 0, 0};
    FILETIME oldTime{};
    const BOOL converted = SystemTimeToFileTime(&oldDate, &oldTime);
    const BOOL changed = converted && SetFileTime(oldFile, nullptr, nullptr, &oldTime);
    CloseHandle(oldFile);
    require(changed != FALSE, "set old date fixture");
    auto scope = shellItem(scopePath);
    const std::vector<std::wstring> queries{
        L"System.Kind:=System.Kind#Document", L"System.Kind:=System.Kind#Picture",
        L"System.Kind:=System.Kind#Music", L"System.Kind:=System.Kind#Video",
        L"System.Kind:=System.Kind#Folder", L"System.Kind:=System.Kind#Program",
        L"System.DateModified:System.StructuredQueryType.DateTime#Today",
        L"System.DateModified:System.StructuredQueryType.DateTime#Yesterday",
        L"System.DateModified:System.StructuredQueryType.DateTime#ThisWeek",
        L"System.DateModified:System.StructuredQueryType.DateTime#LastWeek",
        L"System.DateModified:System.StructuredQueryType.DateTime#ThisMonth",
        L"System.DateModified:System.StructuredQueryType.DateTime#ThisYear",
        L"System.Size:System.Size#Empty", L"System.Size:System.Size#Tiny",
        L"System.Size:System.Size#Small", L"System.Size:System.Size#Medium",
        L"System.Size:System.Size#Large", L"System.Size:System.Size#Huge",
        L"System.Size:System.Size#Gigantic",
        L"(System.Kind:=System.Kind#Document) AND System.Size:>1",
        L"(System.FileName:=\"current.txt\" OR System.FileName:=\"old.txt\") AND NOT System.Size:<1",
        L"System.Size:>=1", L"System.Size:<=100", L"System.Size:<>0"
    };
    unsigned index = 0;
    for (const auto& query : queries) {
        ComPtr<IShellItem> live;
        succeeded(explorer::createSearchFolder(query, scope.Get(), &live), "create canonical ribbon refinement");
        const auto savedPath = fixture.root / (std::to_wstring(++index) + L".search-ms");
        succeeded(explorer::saveSearch(query, scope.Get(), true, savedPath), "save canonical ribbon refinement");
        const auto document = loadXml(savedPath);
        ComPtr<IShellItem> saved;
        try { saved = reopenSearch(savedPath); }
        catch (...) { std::cerr << "canonical refinement fixture number " << index << '\n'; throw; }
        requireResults(searchResults(saved.Get()), searchResults(live.Get()), "canonical saved refinement changed native result semantics");
        if (query.find(L"System.StructuredQueryType.DateTime#") != std::wstring::npos) {
            const auto condition = xmlElement(document.Get(), L"/persistedQuery/query/conditions/condition");
            require(xmlAttribute(condition.Get(), L"valuetype") == L"System.StructuredQueryType.DateTime", "saved relative date lost semantic type");
            const auto value = xmlAttribute(condition.Get(), L"value");
            require(!value.empty() && value.front() == L'R', "saved relative date was frozen to an absolute timestamp");
        }
    }
    const std::wstring today = L"System.DateModified:System.StructuredQueryType.DateTime#Today";
    const auto todayPath = fixture.root / L"today.search-ms";
    succeeded(explorer::saveSearch(today, scope.Get(), false, todayPath), "save relative Today text search");
    requireResults(searchResults(reopenSearch(todayPath).Get()),
        std::set<std::wstring>{(scopePath / L"current.txt").native()}, "native relative Today query did not exclude old fixture");
}

void xmlEscapingAndThisPcScope() {
    Fixture fixture;
    auto scope = shellItem(fixture.root);
    const std::wstring query = L"\"猫 & \"\"quoted\"\" < > ' \U0001F680\"";
    const auto path = fixture.root / L"escaped.search-ms";
    succeeded(explorer::saveSearch(query, scope.Get(), true, path), "save escaped Unicode query");
    const auto bytes = read(path);
    for (const auto* escape : {"&amp;", "&quot;", "&lt;", "&gt;", "&apos;"})
        require(bytes.find(escape) != std::string::npos, "XML metacharacter was not escaped");
    const auto document = loadXml(path);
    const auto condition = xmlElement(document.Get(), L"/persistedQuery/query/conditions/condition");
    require(xmlAttribute(condition.Get(), L"value") == L"猫 & \"quoted\" < > ' \U0001F680", "XML escaping changed literal query text");
    const auto saved = reopenSearch(path);
    require(searchResults(saved.Get()).empty(), "unmatched literal query was broadened while saving");

    const auto computerPath = fixture.root / L"this-pc.search-ms";
    succeeded(explorer::saveSearch(L"unique-no-results-7EB8D806", nullptr, true, computerPath), "save This PC query");
    const auto computerDocument = loadXml(computerPath);
    const auto include = xmlElement(computerDocument.Get(), L"/persistedQuery/query/scope/include");
    wchar_t identifier[40]{};
    require(StringFromGUID2(FOLDERID_ComputerFolder, identifier, 40) != 0, "format This PC known-folder id");
    require(xmlAttribute(include.Get(), L"knownFolder") == identifier, "saved This PC scope used a filesystem substitute");
    reopenSearch(computerPath); // Bind/identity only; never enumerate userwide results.
    const auto namedDirectory = fixture.root / L"ordinary-directory.search-ms";
    require(fs::create_directory(namedDirectory), "create ordinary directory with search-ms suffix");
    ComPtr<IShellItem2> directory;
    auto directoryItem = shellItem(namedDirectory);
    succeeded(directoryItem.As(&directory), "read suffixed directory metadata");
    PWSTR itemType = nullptr;
    const auto typed = directory->GetString(PKEY_ItemType, &itemType);
    const bool isSearch = SUCCEEDED(typed) && itemType && _wcsicmp(itemType, L".search-ms") == 0;
    CoTaskMemFree(itemType);
    succeeded(typed, "read suffixed directory item type");
    require(!isSearch, "ordinary directory suffix was classified as saved search");
}

void comparisonOperatorSemantics() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"scope";
    require(fs::create_directory(scopePath), "create comparison fixture");
    const auto target = scopePath / L"target.txt";
    const auto other = scopePath / L"other.txt";
    const auto empty = scopePath / L"empty.txt";
    write(target, "target"); write(other, std::string(100, 'o')); write(empty, "");
    auto scope = shellItem(scopePath);
    struct Example { const wchar_t* query; std::set<std::wstring> expected; };
    const std::vector<Example> examples{
        {L"System.Size:=6", {target.native()}},
        {L"System.Size:<>0", {target.native(), other.native()}},
        {L"System.Size:<6", {empty.native()}},
        {L"System.Size:>6", {other.native()}},
        {L"System.Size:<=6", {target.native(), empty.native()}},
        {L"System.Size:>=10", {other.native()}},
        {L"System.FileName:~<\"tar\"", {target.native()}},
        {L"System.FileName:~>\".txt\"", {target.native(), other.native(), empty.native()}},
        {L"System.FileName:~=\"arg\"", {target.native()}},
        {L"System.FileName:~!\"arg\"", {other.native(), empty.native()}},
        {L"System.FileName:~\"t*.txt\"", {target.native()}},
        {L"System.FileName:$=\"target\"", {target.native()}},
        {L"(System.FileName:=\"target.txt\" OR System.FileName:=\"other.txt\") AND NOT System.Size:<1",
            {target.native(), other.native()}}
    };
    unsigned index = 0;
    for (const auto& example : examples) {
        ComPtr<IShellItem> live;
        succeeded(explorer::createSearchFolder(example.query, scope.Get(), &live), "create comparison search");
        requireResults(searchResults(live.Get()), example.expected, "live comparison fixture has unexpected results");
        const auto path = fixture.root / (std::to_wstring(++index) + L".search-ms");
        succeeded(explorer::saveSearch(example.query, scope.Get(), true, path), "save native comparison operator");
        loadXml(path);
        requireResults(searchResults(reopenSearch(path).Get()), example.expected, "saved comparison spelling changed its result semantics");
    }
}

void rejectedInputsAndNoOverwrite() {
    Fixture fixture;
    auto scope = shellItem(fixture.root);
    const auto path = fixture.root / L"existing.search-ms";
    write(path, "existing file must survive");
    const auto hr = explorer::saveSearch(L"report", scope.Get(), true, path);
    require(hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "saving replaced an existing file");
    require(read(path) == "existing file must survive", "no-overwrite search changed file bytes");
    const auto rejected = fixture.root / L"rejected.search-ms";
    require(explorer::saveSearch(L" \t\r\n", scope.Get(), true, rejected) == E_INVALIDARG, "empty query accepted");
    require(explorer::saveSearch(std::wstring(1, static_cast<wchar_t>(0xD800)), scope.Get(), true, rejected) == E_INVALIDARG, "invalid UTF-16 query accepted");
    require(explorer::saveSearch(std::wstring(L"a\0b", 3), scope.Get(), true, rejected) == E_INVALIDARG, "embedded NUL query accepted");
    require(explorer::saveSearch(L"report", scope.Get(), true, fixture.root / L"wrong.txt") == E_INVALIDARG, "non-search-ms output accepted");
    require(explorer::saveSearch(L"report", scope.Get(), true, {}) == E_INVALIDARG, "empty output path accepted");
    require(explorer::saveSearch(L"System.FileName:$<\"match\"", scope.Get(), true, rejected) == unsupported, "unverified prefix-word serializer operator guessed");
    require(!fs::exists(rejected), "rejected query left a saved-search file");
    write(fixture.root / L"ordinary-file.txt", "not a folder");
    auto file = shellItem(fixture.root / L"ordinary-file.txt");
    require(explorer::saveSearch(L"report", file.Get(), true, rejected) == HRESULT_FROM_WIN32(ERROR_DIRECTORY), "file used as search scope");
    ComPtr<IShellItem> result;
    require(explorer::createSearchFolder(L"report", nullptr, &result, false) == unsupported && !result, "shallow virtual scope was silently recursive");
    require(explorer::saveSearch(L"report", nullptr, false, rejected) == unsupported, "saved shallow virtual scope was silently recursive");
    require(explorer::createSearchFolder(L"report", scope.Get(), nullptr) == E_POINTER, "null search output accepted");
    require(explorer::createSearchFolder(L"", scope.Get(), &result) == E_INVALIDARG && !result, "invalid live search retained result");
    // Exercise only parsing and the guard; never enumerate the rejected native
    // filename word-prefix condition on affected Windows 10 implementations.
    for (const auto* query : {
        L"System.FileName:$<\"match\"", L"System.FILENAME:$<\"match\"",
        L"System.Kind:=System.Kind#Document AND System.FileName:$<\"match\"",
        L"System.FileName:$<\"match\" OR System.Size:>1",
        L"NOT System.FileName:$<\"match\""}) {
        result.Reset();
        require(explorer::createSearchFolder(query, scope.Get(), &result) == unsupported && !result,
                "explicit filename word-prefix escaped parsed-condition guard");
    }
    for (const auto* query : {L"\"literal $< text\"", L"System.FileName:\"literal $< text\"",
                              L"System.Search.Contents:$<\"match\"", L"ordinary default term"}) {
        result.Reset();
        succeeded(explorer::createSearchFolder(query, scope.Get(), &result),
                  "filename prefix guard rejected unrelated property or literal/default text");
        require(result != nullptr, "permitted search has no native identity");
    }
    const auto percentPath = fixture.root / L"literal %USERPROFILE% scope";
    require(fs::create_directory(percentPath), "create literal percent scope");
    auto percentScope = shellItem(percentPath);
    require(explorer::saveSearch(L"report", percentScope.Get(), true, rejected) == unsupported, "literal percent scope would be environment-expanded when reopened");
    require(!fs::exists(rejected), "unsupported scope left a partial saved search");
}
} // namespace

int runSearchTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"native live/saved recursive and shallow fixture results", shallowAndRecursiveResults},
        {"canonical kind/date/size refinements and relative dates", canonicalRefinementsAndRelativeDates},
        {"native saved numeric/string/wildcard/Boolean comparison results", comparisonOperatorSemantics},
        {"saved-search XML escaping, Unicode and This PC identity", xmlEscapingAndThisPcScope},
        {"saved-search invalid input, unsupported scope and no overwrite", rejectedInputsAndNoOverwrite}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    std::cout << tests.size() - static_cast<size_t>(failures) << '/' << tests.size() << " headless native search groups passed\n";
    return failures;
}
