#include "explorer/search.hpp"
#include "explorer/saved_search.hpp"

#include <shlobj.h>
#include <shlguid.h>
#include <propkey.h>
#include <sddl.h>
#include <winioctl.h>
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
void requireReopenedResults(const fs::path& path, const std::set<std::wstring>& expected,
                            const char* message) {
    std::set<FileIdentity> expectedIds;
    for (const auto& value : expected) expectedIds.insert(fileIdentity(value));
    const auto started = GetTickCount64();
    unsigned retries = 0;
    for (;;) {
        const auto actual = searchResults(reopenSearch(path).Get());
        std::set<FileIdentity> actualIds;
        for (const auto& value : actual) actualIds.insert(fileIdentity(value));
        if (actual.size() == expected.size() && actualIds == expectedIds && actual.size() == actualIds.size()) {
            if (retries) std::cout << "INFO: native replaced-query notification settled after " << retries
                                  << " retries / " << GetTickCount64() - started << "ms\n";
            return;
        }
        if (GetTickCount64() - started >= 5000) {
            requireResults(actual, expected, message);
            return;
        }
        // Native query/cache notifications run on the owning STA. The real
        // app keeps pumping; this windowless fixture must do the same.
        MSG event{};
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&event); DispatchMessageW(&event);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        ++retries;
    }
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
        L"System.DateModified:System.StructuredQueryType.DateTime#LastMonth",
        L"System.DateModified:System.StructuredQueryType.DateTime#ThisYear",
        L"System.DateModified:System.StructuredQueryType.DateTime#LastYear",
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
    const auto relativeRangePath = fixture.root / L"relative-range.search-ms";
    succeeded(explorer::saveSearch(L"System.DateModified:System.StructuredQueryType.DateTime#Today..System.StructuredQueryType.DateTime#Tomorrow",
        scope.Get(), true, relativeRangePath), "save unresolved relative date range");
    explorer::SavedSearchMetadata relativeRange;
    succeeded(explorer::readSavedSearch(relativeRangePath, &relativeRange), "restore unresolved relative date range");
    const auto expectedToday = std::set<std::wstring>{(scopePath / L"current.txt").native()};
    requireResults(searchResults(reopenSearch(relativeRangePath).Get()), expectedToday,
                   "saved relative range included an old fixture or omitted current identity");
    ComPtr<IShellItem> liveRelativeRange;
    succeeded(explorer::createSearchFolder(relativeRange.query, relativeRange.scope.Get(), &liveRelativeRange),
              "rebuild unresolved relative range");
    requireResults(searchResults(liveRelativeRange.Get()), expectedToday, "restored relative range changed native identities");
    const auto relativeRangeAgain = fixture.root / L"relative-range-again.search-ms";
    succeeded(explorer::saveSearch(relativeRange.query, relativeRange.scope.Get(), true, relativeRangeAgain),
              "resave unresolved relative range");
    for (const auto& path : {relativeRangePath, relativeRangeAgain}) {
        auto document = loadXml(path);
        const auto first = xmlElement(document.Get(), L"/persistedQuery/query/conditions/condition/condition[1]");
        const auto last = xmlElement(document.Get(), L"/persistedQuery/query/conditions/condition/condition[2]");
        for (const auto& bound : {first, last}) {
            const auto token = xmlAttribute(bound.Get(), L"value");
            require(xmlAttribute(bound.Get(), L"valuetype") == L"System.StructuredQueryType.DateTime" &&
                    !token.empty() && token.front() == L'R', "relative date range was frozen while saving or re-saving");
        }
    }
}

ULONGLONG localTimestamp(WORD year, WORD month, WORD day, WORD hour = 0) {
    SYSTEMTIME local{};
    local.wYear = year; local.wMonth = month; local.wDay = day; local.wHour = hour;
    SYSTEMTIME utc{};
    require(TzSpecificLocalTimeToSystemTimeEx(nullptr, &local, &utc) != FALSE,
            "convert owned civil date using current native time-zone rules");
    FILETIME value{};
    require(SystemTimeToFileTime(&utc, &value) != FALSE, "encode owned native UTC file timestamp");
    return (static_cast<ULONGLONG>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
void setOwnedTimestamp(const fs::path& path, ULONGLONG ticks) {
    const auto file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "open owned date-boundary fixture");
    const FILETIME requested{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    FILETIME actual{};
    const bool changed = SetFileTime(file, nullptr, nullptr, &requested) != FALSE &&
                         GetFileTime(file, nullptr, nullptr, &actual) != FALSE;
    CloseHandle(file);
    require(changed && actual.dwHighDateTime == requested.dwHighDateTime &&
            actual.dwLowDateTime == requested.dwLowDateTime, "owned filesystem lost exact 100ns date boundary");
}
ULONGLONG ownedWriteTimestamp(const fs::path& path) {
    const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "read owned date fixture timestamp");
    FILETIME actual{};
    const bool readTime = GetFileTime(file, nullptr, nullptr, &actual) != FALSE;
    CloseHandle(file);
    require(readTime, "read exact native date fixture timestamp");
    return (static_cast<ULONGLONG>(actual.dwHighDateTime) << 32) | actual.dwLowDateTime;
}
std::wstring utcTimestampText(ULONGLONG ticks) {
    const FILETIME value{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    require(FileTimeToSystemTime(&value, &utc) != FALSE, "decode owned UTC query boundary");
    wchar_t text[40]{};
    swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay,
               utc.wHour, utc.wMinute, utc.wSecond);
    return text;
}
void absoluteDateDayAndRangeResults() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"owned timestamp scope";
    require(fs::create_directory(scopePath), "create exclusively owned date scope");
    const auto leapStart = localTimestamp(2024, 2, 29);
    const auto marchStart = localTimestamp(2024, 3, 1);
    const auto rangeEnd = localTimestamp(2024, 3, 3);
    const auto yearStart = localTimestamp(2024, 12, 31);
    const auto januaryStart = localTimestamp(2025, 1, 1);
    const auto januaryEnd = localTimestamp(2025, 1, 2);
    const auto springStart = localTimestamp(2024, 3, 31);
    const auto springEnd = localTimestamp(2024, 4, 1);
    const auto autumnStart = localTimestamp(2024, 10, 27);
    const auto autumnEnd = localTimestamp(2024, 10, 28);
    struct Item { const wchar_t* name; ULONGLONG ticks; const char* contents; };
    const std::array<Item, 17> items{{
        {L"before-leap.bin", leapStart - 1, "p"},
        {L"leap-start.bin", leapStart, "a"},
        {L"leap-midday.bin", localTimestamp(2024, 2, 29, 12), "bb"},
        {L"leap-last-100ns.bin", marchStart - 1, "ccc"},
        {L"march-start.bin", marchStart, "dddd"},
        {L"range-last-100ns.bin", rangeEnd - 1, "eeeee"},
        {L"after-range.bin", rangeEnd, "ffffff"},
        {L"year-start.bin", yearStart, "ggggggg"},
        {L"year-last-100ns.bin", januaryStart - 1, "hhhhhhhh"},
        {L"january-start.bin", januaryStart, "iiiiiiiii"},
        {L"january-last-100ns.bin", januaryEnd - 1, "jjjjjjjjjj"},
        {L"spring-start.bin", springStart, "spring"},
        {L"spring-last-100ns.bin", springEnd - 1, "spring end"},
        {L"after-spring.bin", springEnd, "after spring"},
        {L"autumn-start.bin", autumnStart, "autumn"},
        {L"autumn-last-100ns.bin", autumnEnd - 1, "autumn end"},
        {L"after-autumn.bin", autumnEnd, "after autumn"}}};
    std::vector<FileIdentity> originalIds;
    for (const auto& item : items) {
        const auto path = scopePath / item.name;
        write(path, item.contents);
        setOwnedTimestamp(path, item.ticks);
        originalIds.push_back(fileIdentity(path.native()));
    }
    auto scope = shellItem(scopePath);
    const auto scopeId = fileIdentity(scopePath.native());
    const auto known = [&](std::initializer_list<unsigned> indices) {
        std::set<std::wstring> paths;
        for (const auto index : indices) paths.insert((scopePath / items[index].name).native());
        return paths;
    };
    struct Example { std::wstring query; std::set<std::wstring> expected; };
    const std::vector<Example> examples{
        {L"System.DateModified:2024-02-29", known({1, 2, 3})},
        {L"System.DateModified:=2024-02-29", known({1, 2, 3})},
        {L"System.DateModified:2024-02-29..2024-03-02", known({1, 2, 3, 4, 5})},
        {L"System.DateModified:=2024-02-29..2024-03-02", known({1, 2, 3, 4, 5})},
        {L"NOT System.DateModified:2024-02-29..2024-03-02", known({0, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})},
        {L"System.DateModified:(>=2024-02-29T00:00:00 AND <2024-03-03T00:00:00)", known({1, 2, 3, 4, 5})},
        {L"(System.DateModified:(>=2024-02-29T00:00:00 AND <2024-03-03T00:00:00)) AND System.Size:>1", known({2, 3, 4, 5})},
        {L"System.DateModified:(>=" + utcTimestampText(leapStart) + L" AND <" + utcTimestampText(marchStart) + L")", known({1, 2, 3})},
        {L"System.DateModified:2024-12-31..2025-01-01", known({7, 8, 9, 10})},
        {L"System.DateModified:<2024-02-29", known({0})},
        {L"System.DateModified:<=2024-02-29", known({0, 1, 2, 3})},
        {L"System.DateModified:>2024-02-29", known({4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})},
        {L"NOT System.DateModified:2024-02-29", known({0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})},
        {L"System.DateModified:=2024-03-31", known({11, 12})},
        {L"System.DateModified:2024-03-31..2024-03-31", known({11, 12})},
        {L"System.DateModified:2024-10-27", known({14, 15})},
        {L"System.DateModified:(>=" + utcTimestampText(springStart) + L" AND <" + utcTimestampText(springEnd) + L")", known({11, 12})}};
    const auto verify = [&](IShellItem* search, const std::set<std::wstring>& expected) {
        const auto actual = searchResults(search);
        for (const auto& path : actual)
            require(fileIdentity(fs::path(path).parent_path().native()) == scopeId,
                    "date search escaped its exclusively owned native scope");
        requireResults(actual, expected, "native date query changed exact owned membership or day boundary");
    };
    unsigned index = 0;
    for (const auto& example : examples) {
        try {
            const auto originalText = example.query;
            ComPtr<IShellItem> live;
            succeeded(explorer::createSearchFolder(example.query, scope.Get(), &live), "create native typed day/range query");
            verify(live.Get(), example.expected);
            const auto saved = fixture.root / (L"day-range-" + std::to_wstring(++index) + L".search-ms");
            succeeded(explorer::saveSearch(example.query, scope.Get(), true, saved), "save native typed date query");
            verify(reopenSearch(saved).Get(), example.expected);
            explorer::SavedSearchMetadata metadata;
            succeeded(explorer::readSavedSearch(saved, &metadata), "restore native typed date metadata");
            require(metadata.recursive && !metadata.query.empty() && metadata.scopeRules.size() == 1 &&
                    metadata.scopeRules.front().recursive && !metadata.scopeRules.front().excluded,
                    "date metadata changed original scope or recursion");
            ComPtr<IShellItem> restored;
            succeeded(explorer::createSearchFolderForScopeRules(metadata.query, metadata.scopeRules, &restored),
                      "rebuild native query from restored typed date metadata");
            verify(restored.Get(), example.expected);
            const auto again = fixture.root / (L"day-range-again-" + std::to_wstring(index) + L".search-ms");
            succeeded(explorer::saveSearchForScopeRules(metadata.query, metadata.scopeRules, again), "resave restored typed date metadata");
            verify(reopenSearch(again).Get(), example.expected);
            require(example.query == originalText, "native date parsing modified supplied query text");
        } catch (...) {
            std::cerr << "date query case " << index << ": " << utf8(example.query) << '\n';
            throw;
        }
    }
    const auto validSaved = fixture.root / L"day-range-1.search-ms";
    const auto originalSavedBytes = read(validSaved);
    const auto originalSavedId = fileIdentity(validSaved.native());
    explorer::SavedSearchMetadata preserved;
    succeeded(explorer::readSavedSearch(validSaved, &preserved), "read metadata sentinel before invalid date input");
    const auto originalQuery = preserved.query;
    const auto* originalScope = preserved.scope.Get();
    const auto* originalScopes = preserved.scopes.Get();
    const auto originalRules = preserved.scopeRules;
    unsigned invalidIndex = 0;
    for (const std::wstring invalidDate : {L"System.DateModified:2024-02-30", L"System.DateModified:2023-02-29"}) {
        const auto unchangedInput = invalidDate;
        // Windows' tolerant parser treats an invalid date as a Value/generic
        // condition. Do not invent date normalization or execute that fallback.
        const auto invalidSaved = fixture.root / (L"invalid-date-" + std::to_wstring(++invalidIndex) + L".search-ms");
        const auto saved = explorer::saveSearch(invalidDate, scope.Get(), true, invalidSaved);
        if (SUCCEEDED(saved)) {
            require(FAILED(explorer::readSavedSearch(invalidSaved, &preserved)),
                    "invalid-date metadata was silently changed into a valid date");
        } else require(!fs::exists(invalidSaved), "rejected invalid date left a partial saved search");
        require(preserved.query == originalQuery && preserved.scope.Get() == originalScope &&
                preserved.scopes.Get() == originalScopes && preserved.recursive &&
                preserved.scopeRules.size() == originalRules.size() &&
                preserved.scopeRules.front().folder.Get() == originalRules.front().folder.Get() &&
                preserved.scopeRules.front().recursive == originalRules.front().recursive &&
                preserved.scopeRules.front().excluded == originalRules.front().excluded,
                "failed invalid-date metadata import changed original caller fields");
        require(FAILED(explorer::saveSearch(invalidDate, scope.Get(), true, validSaved)) &&
                read(validSaved) == originalSavedBytes && fileIdentity(validSaved.native()) == originalSavedId,
                "invalid-date save overwrote an existing valid query");
        require(invalidDate == unchangedInput, "invalid-date parsing changed supplied query text");
    }
    for (size_t member = 0; member < items.size(); ++member) {
        const auto path = scopePath / items[member].name;
        require(fileIdentity(path.native()) == originalIds[member] && read(path) == items[member].contents &&
                ownedWriteTimestamp(path) == items[member].ticks,
                "date queries modified owned file identity/content or exact timestamp");
    }
    std::cout << "INFO: date-range fixture cases=" << examples.size() << " identities=" << items.size()
              << " nativeSpringDayHours=" << (springEnd - springStart) / 36000000000ULL
              << " nativeAutumnDayHours=" << (autumnEnd - autumnStart) / 36000000000ULL << '\n';
}

void xmlEscapingAndThisPcScope() {
    Fixture fixture;
    auto scope = shellItem(fixture.root);
    const std::wstring query = L"System.Title:=\"猫 & \"\"quoted\"\" < > ' \U0001F680\"";
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
    succeeded(explorer::saveSearch(L"System.FileName:=\"unique-no-results-7EB8D806\"", nullptr, true, computerPath), "save This PC query");
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
    const auto hr = explorer::saveSearch(L"System.FileName:=\"report\"", scope.Get(), true, path);
    require(hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "saving replaced an existing file");
    require(read(path) == "existing file must survive", "no-overwrite search changed file bytes");
    const auto rejected = fixture.root / L"rejected.search-ms";
    require(explorer::saveSearch(L" \t\r\n", scope.Get(), true, rejected) == E_INVALIDARG, "empty query accepted");
    require(explorer::saveSearch(std::wstring(1, static_cast<wchar_t>(0xD800)), scope.Get(), true, rejected) == E_INVALIDARG, "invalid UTF-16 query accepted");
    require(explorer::saveSearch(std::wstring(L"a\0b", 3), scope.Get(), true, rejected) == E_INVALIDARG, "embedded NUL query accepted");
    require(explorer::saveSearch(L"System.FileName:=\"report\"", scope.Get(), true, fixture.root / L"wrong.txt") == E_INVALIDARG, "non-search-ms output accepted");
    require(explorer::saveSearch(L"System.FileName:=\"report\"", scope.Get(), true, {}) == E_INVALIDARG, "empty output path accepted");
    require(explorer::saveSearch(L"System.FileName:$<\"match\"", scope.Get(), true, rejected) == unsupported, "unverified prefix-word serializer operator guessed");
    require(!fs::exists(rejected), "rejected query left a saved-search file");
    write(fixture.root / L"ordinary-file.txt", "not a folder");
    auto file = shellItem(fixture.root / L"ordinary-file.txt");
    require(explorer::saveSearch(L"System.FileName:=\"report\"", file.Get(), true, rejected) == HRESULT_FROM_WIN32(ERROR_DIRECTORY), "file used as search scope");
    ComPtr<IShellItem> result;
    require(explorer::createSearchFolder(L"report", nullptr, &result, false) == unsupported && !result, "shallow virtual scope was silently recursive");
    require(explorer::saveSearch(L"System.FileName:=\"report\"", nullptr, false, rejected) == unsupported, "saved shallow virtual scope was silently recursive");
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
    require(explorer::saveSearch(L"System.FileName:=\"report\"", percentScope.Get(), true, rejected) == unsupported, "literal percent scope would be environment-expanded when reopened");
    require(!fs::exists(rejected), "unsupported scope left a partial saved search");
}

struct OwnedHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~OwnedHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring fileDacl(const fs::path& path) {
    DWORD length = 0;
    GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &length);
    require(length != 0, "read owned saved-search DACL size");
    std::vector<BYTE> security(length);
    require(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, security.data(), length, &length) != FALSE,
            "read owned saved-search DACL");
    PWSTR raw = nullptr;
    require(ConvertSecurityDescriptorToStringSecurityDescriptorW(security.data(), SDDL_REVISION_1,
        DACL_SECURITY_INFORMATION, &raw, nullptr) != FALSE && raw, "format owned saved-search DACL");
    const std::wstring result(raw);
    LocalFree(raw);
    return result;
}
FILE_BASIC_INFO fileBasic(const fs::path& path) {
    OwnedHandle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    FILE_BASIC_INFO result{};
    require(file.value != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandleEx(file.value, FileBasicInfo, &result, sizeof(result)), "read owned saved-search basic metadata");
    return result;
}
std::set<std::wstring> fixtureMembers(const fs::path& root) {
    std::set<std::wstring> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) paths.insert(entry.path().lexically_relative(root).native());
    return paths;
}
void confirmedSearchReplacement() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"scope";
    require(fs::create_directory(scopePath), "create owned replacement search scope");
    const auto first = scopePath / L"first.txt", second = scopePath / L"second.bin";
    write(first, "original source must survive"); write(second, "second source must survive");
    const auto firstId = fileIdentity(first.native()), secondId = fileIdentity(second.native());
    auto scope = shellItem(scopePath);
    const auto output = fixture.root / L"Saved-資料.search-ms";
    succeeded(explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output), "create original saved query");
    const auto originalId = fileIdentity(output.native());
    const auto originalBytes = read(output);
    require(SetFileAttributesW(output.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) != FALSE,
            "set owned saved-query attributes");
    const auto dacl = fileDacl(output);
    const auto basic = fileBasic(output);
    const auto members = fixtureMembers(fixture.root);
    succeeded(explorer::saveSearch(L"System.FileName:=\"second.bin\"", scope.Get(), true, output,
        explorer::SearchSaveMode::UserConfirmed), "replace user-confirmed saved query with complete native XML");
    const auto replacementBytes = read(output);
    if (replacementBytes == originalBytes) {
        std::cerr << "confirmed replacement bytes=" << replacementBytes.size()
                  << " firstToken=" << (replacementBytes.find("first.txt") != std::string::npos)
                  << " secondToken=" << (replacementBytes.find("second.bin") != std::string::npos)
                  << " sameIdentity=" << (fileIdentity(output.native()) == originalId) << '\n';
    }
    require(replacementBytes != originalBytes, "confirmed replacement did not publish the new complete query");
    std::cout << "INFO: native confirmed save retained prior filename identity=" <<
        (fileIdentity(output.native()) == originalId) << '\n';
    require(fileDacl(output) == dacl, "confirmed replacement changed the existing saved-query DACL");
    const auto replaced = fileBasic(output);
    require(replaced.CreationTime.QuadPart == basic.CreationTime.QuadPart && replaced.FileAttributes == basic.FileAttributes,
            "confirmed replacement lost existing creation time or attributes");
    require(fixtureMembers(fixture.root) == members, "confirmed replacement leaked a temporary/backup file");
    loadXml(output);
    requireReopenedResults(output, {second.native()}, "replaced native saved query has stale membership");
    explorer::SavedSearchMetadata imported;
    succeeded(explorer::readSavedSearch(output, &imported), "read replaced saved-query metadata");
    require(imported.query.find(L"second.bin") != std::wstring::npos, "replaced query retained stale metadata");
    const auto preservedBytes = read(output);
    const auto preservedId = fileIdentity(output.native());
    const auto preservedBasic = fileBasic(output);
    const auto preserved = [&] {
        require(read(output) == preservedBytes && fileIdentity(output.native()) == preservedId,
                "failed replacement changed original bytes or identity");
        const auto now = fileBasic(output);
        require(now.CreationTime.QuadPart == preservedBasic.CreationTime.QuadPart &&
                now.LastWriteTime.QuadPart == preservedBasic.LastWriteTime.QuadPart &&
                now.ChangeTime.QuadPart == preservedBasic.ChangeTime.QuadPart &&
                now.FileAttributes == preservedBasic.FileAttributes && fileDacl(output) == dacl,
                "failed replacement changed original metadata or permissions");
        require(fixtureMembers(fixture.root) == members, "failed replacement left temporary artifacts");
    };
    require(FAILED(explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output)),
            "default saved-search API overwrote the existing file");
    preserved();
    require(explorer::saveSearch(L"", scope.Get(), true, output, explorer::SearchSaveMode::UserConfirmed) == E_INVALIDARG,
            "confirmed mode accepted invalid query");
    preserved();
    require(explorer::saveSearch(L"System.Size:>0", scope.Get(), true, output,
        static_cast<explorer::SearchSaveMode>(99)) == E_INVALIDARG, "unknown saved-search publication mode accepted");
    preserved();
    {
        OwnedHandle locked{CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(locked.value != INVALID_HANDLE_VALUE, "lock owned query against replacement");
        const auto hr = explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output,
            explorer::SearchSaveMode::UserConfirmed);
        if (hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
            std::cerr << "locked confirmed replacement HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
        require(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "locked replacement did not preserve native sharing failure");
        preserved();
    }
    require(SetFileAttributesW(output.c_str(), basic.FileAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
            "make owned saved query read-only");
    const auto readOnlyId = fileIdentity(output.native());
    const auto readOnlyBasic = fileBasic(output);
    require(explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output,
        explorer::SearchSaveMode::UserConfirmed) == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
        "confirmed replacement overrode the existing read-only attribute");
    const auto afterReadOnly = fileBasic(output);
    require(read(output) == preservedBytes && fileIdentity(output.native()) == readOnlyId &&
            afterReadOnly.ChangeTime.QuadPart == readOnlyBasic.ChangeTime.QuadPart &&
            afterReadOnly.FileAttributes == readOnlyBasic.FileAttributes && fixtureMembers(fixture.root) == members,
            "failed read-only replacement changed the original or created artifacts");
    require(SetFileAttributesW(output.c_str(), basic.FileAttributes) != FALSE, "restore only owned query attributes");

    // Exercise the same publication through union/rule entry points, including
    // an absent path after a successful Save dialog selected a fresh name.
    ComPtr<IShellItemArray> scopes;
    succeeded(SHCreateShellItemArrayFromShellItem(scope.Get(), IID_PPV_ARGS(&scopes)), "create owned union scope");
    succeeded(explorer::saveSearchForScopes(L"System.FileName:=\"first.txt\"", scopes.Get(), true, output,
        explorer::SearchSaveMode::UserConfirmed), "replace confirmed union-scope query");
    require(read(output).find("first.txt") != std::string::npos && read(output).find("second.bin") == std::string::npos,
            "union replacement did not publish the requested XML");
    requireReopenedResults(output, {first.native()}, "union replacement kept stale query membership");
    const std::vector<explorer::SearchScopeRule> rules{{scope, false, false}};
    succeeded(explorer::saveSearchForScopeRules(L"System.FileName:=\"second.bin\"", rules, output,
        explorer::SearchSaveMode::UserConfirmed), "replace confirmed explicit scope-rule query");
    requireReopenedResults(output, {second.native()}, "rule replacement changed native scope semantics");
    const auto fresh = fixture.root / L"new-confirmed.search-ms";
    succeeded(explorer::saveSearchForScopeRules(L"System.Size:>0", rules, fresh,
        explorer::SearchSaveMode::UserConfirmed), "confirmed dialog fresh path creates a new query");
    requireResults(searchResults(reopenSearch(fresh).Get()), {first.native(), second.native()}, "fresh confirmed save lost native results");

    // Saved XML is small; native filesystem compression can be retained without
    // reading the old file's content or manipulating any user certificate.
    {
        OwnedHandle file{CreateFileW(output.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        USHORT compression = COMPRESSION_FORMAT_DEFAULT;
        DWORD returned = 0;
        require(file.value != INVALID_HANDLE_VALUE && DeviceIoControl(file.value, FSCTL_SET_COMPRESSION,
            &compression, sizeof(compression), nullptr, 0, &returned, nullptr), "compress only owned saved-query fixture");
    }
    const auto compressed = fileBasic(output);
    require((compressed.FileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0, "owned query compression was not applied");
    succeeded(explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output,
        explorer::SearchSaveMode::UserConfirmed), "replace compressed confirmed query preserving native compression");
    require(read(output).find("first.txt") != std::string::npos && read(output).find("second.bin") == std::string::npos,
            "compressed replacement did not publish the requested XML");
    require((fileBasic(output).FileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0 && fileDacl(output) == dacl,
            "confirmed save lost native compression or permissions");
    requireReopenedResults(output, {first.native()}, "compressed replacement has incorrect native results");
    require(fileIdentity(first.native()) == firstId && fileIdentity(second.native()) == secondId &&
            read(first) == "original source must survive" && read(second) == "second source must survive",
            "saved-query replacement changed source file identity or content");
    require(fixtureMembers(fixture.root).size() == members.size() + 1, "final confirmed saves leaked temporary files");
}
} // namespace

int runSearchTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"native live/saved recursive and shallow fixture results", shallowAndRecursiveResults},
        {"canonical kind/date/size refinements and relative dates", canonicalRefinementsAndRelativeDates},
        {"typed day/range inclusive boundaries and live/saved/restored native identities", absoluteDateDayAndRangeResults},
        {"native saved numeric/string/wildcard/Boolean comparison results", comparisonOperatorSemantics},
        {"saved-search XML escaping, Unicode and This PC identity", xmlEscapingAndThisPcScope},
        {"confirmed saved-search replacement, native results, permissions and failure preservation", confirmedSearchReplacement},
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
