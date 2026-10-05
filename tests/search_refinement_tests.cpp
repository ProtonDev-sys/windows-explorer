#include "explorer/search_refinement.hpp"
#include "explorer/search.hpp"
#include "explorer/saved_search.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <structuredquery.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
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
using Microsoft::WRL::ComPtr;
using explorer::NativeSearchRefinements;
using Category = explorer::SearchRefinementCategory;
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::ostringstream text; text << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr); throw std::runtime_error(text.str()); }
}
std::string diagnosticAscii(const wchar_t* text) {
    if (!text) return "NULL";
    std::string result;
    for (; *text; ++text) result += static_cast<unsigned>(*text) < 128 ? static_cast<char>(*text) : '?';
    return result;
}
struct Identity {
    ULONGLONG volume = 0; std::array<BYTE, 16> file{};
    auto operator<=>(const Identity&) const = default;
};
Identity identity(const fs::path& path) {
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open only owned result identity");
    FILE_ID_INFO id{}; const auto ok = GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)); CloseHandle(handle);
    require(ok != FALSE, "read owned volume/128-bit FileID");
    Identity result{id.VolumeSerialNumber}; std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), result.file.begin()); return result;
}
ComPtr<IShellItem> item(const fs::path& path) {
    ComPtr<IShellItem> result; checked(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&result)), "create owned Shell item"); return result;
}
std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); require(input.good(), "read only owned saved query bytes");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Fixture {
    fs::path root, scope;
    std::array<fs::path, 5> paths;
    std::array<Identity, 5> identities;
    std::array<std::string, 4> contents{"today", "month", "", "outside predicate"};
    Fixture() {
        GUID guid{}; checked(CoCreateGuid(&guid), "owned refinement fixture GUID"); wchar_t name[40]{};
        require(StringFromGUID2(guid, name, 40) != 0, "format owned fixture GUID");
        root = fs::temp_directory_path() / (std::wstring(L"Explorer-native-refinement-") + name);
        require(fs::create_directory(root), "create exclusively owned fixture"); scope = root / L"Scope";
        require(fs::create_directory(scope), "create owned shallow scope");
        paths = {scope / L"target-today.txt", scope / L"target-month.txt", scope / L"target-empty.txt", scope / L"different-month.txt", scope / L"Folder"};
        SYSTEMTIME now{}; GetLocalTime(&now);
        SYSTEMTIME previous = now; previous.wDay = 15; previous.wHour = 12; previous.wMinute = previous.wSecond = previous.wMilliseconds = 0;
        if (previous.wMonth == 1) { --previous.wYear; previous.wMonth = 12; } else --previous.wMonth;
        SYSTEMTIME previousUtc{}; require(TzSpecificLocalTimeToSystemTime(nullptr, &previous, &previousUtc) != FALSE, "native previous-month timestamp");
        FILETIME previousTime{}; require(SystemTimeToFileTime(&previousUtc, &previousTime) != FALSE, "previous month FILETIME");
        for (size_t index = 0; index < 4; ++index) {
            { std::ofstream output(paths[index], std::ios::binary); output << contents[index]; require(output.good(), "write owned refinement member"); }
            if (index == 1 || index == 3) {
                const auto handle = CreateFileW(paths[index].c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                require(handle != INVALID_HANDLE_VALUE, "open owned timestamp"); const auto ok = SetFileTime(handle, nullptr, nullptr, &previousTime); CloseHandle(handle);
                require(ok != FALSE, "stamp real previous-month member");
            }
            identities[index] = identity(paths[index]);
        }
        require(fs::create_directory(paths[4]), "create actual folder Kind member"); identities[4] = identity(paths[4]);
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void unchanged() const {
        for (size_t index = 0; index < paths.size(); ++index) {
            require(identity(paths[index]) == identities[index], "refinement changed source native FileID");
            if (index < 4) { std::ifstream input(paths[index], std::ios::binary); const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                require(bytes == contents[index], "refinement changed owned source contents"); }
        }
    }
};
std::set<Identity> results(IShellItem* search, const Fixture& fixture) {
    ComPtr<IShellFolder> folder; checked(search->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder)), "bind actual native query");
    ComPtr<IEnumIDList> enumeration;
    const auto start = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumeration);
    if (start == S_FALSE) return {}; checked(start, "enumerate actual native results"); require(enumeration != nullptr, "native query enumerator missing");
    std::set<Identity> actual;
    for (unsigned count = 0; ; ++count) {
        require(count < 32, "bounded real owned results"); PITEMID_CHILD child = nullptr; ULONG fetched = 0;
        const auto next = enumeration->Next(1, &child, &fetched); if (next == S_FALSE) { CoTaskMemFree(child); break; }
        checked(next, "read actual native result"); require(child && fetched == 1, "native result identity missing");
        ComPtr<IShellItem> member; const auto create = SHCreateItemWithParent(nullptr, folder.Get(), child, IID_PPV_ARGS(&member)); CoTaskMemFree(child);
        checked(create, "bind actual result parent"); PWSTR raw = nullptr; checked(member->GetDisplayName(SIGDN_FILESYSPATH, &raw), "read owned result path");
        require(raw != nullptr, "native result path missing"); const fs::path path(raw); CoTaskMemFree(raw);
        // TEMP can use an 8.3 alias while the Shell returns a long name.
        // Directory ownership is the actual volume/128-bit identity.
        require(identity(path.parent_path()) == identity(fixture.scope), "native result left exclusively owned shallow scope");
        require(actual.insert(identity(path)).second, "duplicate native refinement result");
    }
    return actual;
}
ComPtr<IShellItem> live(const std::wstring& query, IShellItem* scope) {
    ComPtr<IShellItem> result; checked(explorer::createSearchFolder(query, scope, &result, false), "create actual native refined query"); return result;
}
void exact(const std::function<std::set<Identity>()>& read, const std::set<Identity>& expected,
           const std::string& stage) {
    const auto started = GetTickCount64();
    std::set<Identity> actual;
    do { actual = read(); if (actual == expected) return;
        MSG event{}; while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
        Sleep(20);
    } while (GetTickCount64() - started < 5000);
    std::ostringstream diagnostic;
    diagnostic << "exact native refinement FileID membership mismatch stage=" << stage <<
        " expectedCount=" << expected.size() << " actualCount=" << actual.size();
    const auto identities = [&](const char* label, const std::set<Identity>& values) {
        diagnostic << ' ' << label << "=[";
        for (const auto& value : values) {
            diagnostic << std::hex << value.volume << ':';
            for (const auto byte : value.file) diagnostic << static_cast<unsigned>(byte) << ',';
            diagnostic << ';';
        }
        diagnostic << ']' << std::dec;
    };
    identities("expectedOwnedFileIDs", expected); identities("actualOwnedFileIDs", actual);
    throw std::runtime_error(diagnostic.str());
}
void replacementRoutes() {
    Fixture fixture; const auto scope = item(fixture.scope);
    struct Case { Category category; std::wstring query, preset; std::set<Identity> before, after; };
    const std::wstring filename = L"(System.FileName:~\"target-*.txt\" OR System.FileName:=\"unused.txt\")";
    const std::wstring today = L"System.DateModified:System.StructuredQueryType.DateTime#Today";
    const std::vector<Case> cases{
        {Category::Date, filename + L" AND NOT System.FileName:=\"target-empty.txt\" AND " + today,
            L"System.DateModified:System.StructuredQueryType.DateTime#LastMonth", {fixture.identities[0]}, {fixture.identities[1]}},
        {Category::Size, filename + L" AND System.Size:>0", L"System.Size:System.Size#Empty",
            {fixture.identities[0], fixture.identities[1]}, {fixture.identities[2]}},
        {Category::Kind, L"NOT System.FileName:=\"unused.txt\" AND System.Kind:=System.Kind#Document", L"System.Kind:=System.Kind#Folder",
            {fixture.identities[0], fixture.identities[1], fixture.identities[2], fixture.identities[3]}, {fixture.identities[4]}}
    };
    explorer::SearchFileProperties properties; properties.author = L"Owned 作者"; properties.kind = L"searchfolder";
    properties.description = L"literal & <metadata>"; properties.tags = L"owned;scope";
    explorer::SearchViewPresentation presentation; presentation.mode = explorer::SearchViewMode::Details; presentation.iconSize = 16;
    presentation.visibleColumns = std::vector<std::wstring>{L"System.ItemNameDisplay", L"System.Size"};
    presentation.sort = std::vector<explorer::SearchViewOrder>{{L"System.ItemNameDisplay", SORT_DESCENDING}};
    const std::vector<explorer::SearchScopeRule> rules{{scope, false, false}};
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto& example = cases[index];
        const auto stage = "category-" + std::to_string(index);
        exact([&] { return results(live(example.query, scope.Get()).Get(), fixture); }, example.before, stage + "-original-live");
        std::shared_ptr<NativeSearchRefinements> typed; checked(NativeSearchRefinements::inspect(example.query, &typed), "inspect complete typed query");
        std::wstring typedResult; checked(typed->replace(example.category, example.preset, &typedResult), "replace typed category");
        exact([&] { return results(live(typedResult, scope.Get()).Get(), fixture); }, example.after, stage + "-typed-replacement");
        const auto source = fixture.root / (L"source-" + std::to_wstring(index) + L".search-ms");
        checked(explorer::saveSearchForScopeRules(example.query, rules, source, explorer::SearchSaveMode::CreateNew, &presentation, &properties), "save actual initial category query");
        const auto originalSourceIdentity = identity(source);
        const auto originalSourceBytes = bytes(source);
        exact([&] { return results(item(source).Get(), fixture); }, example.before, stage + "-original-native-saved");
        explorer::SavedSearchMetadata metadata; checked(explorer::readSavedSearch(source, &metadata), "import full native category query");
        require(metadata.presentation && metadata.fileProperties, "native import lost required owned metadata before refinement");
        std::shared_ptr<NativeSearchRefinements> imported; checked(NativeSearchRefinements::inspect(metadata.query, &imported), "inspect imported category query");
        std::wstring changed; checked(imported->replace(example.category, example.preset, &changed), "replace imported category");
        exact([&] { return results(live(changed, scope.Get()).Get(), fixture); }, example.after, stage + "-imported-replacement");
        const auto destination = fixture.root / (L"refined-" + std::to_wstring(index) + L".search-ms");
        checked(explorer::saveSearchForScopeRules(changed, metadata.scopeRules, destination, explorer::SearchSaveMode::CreateNew,
                                                 &*metadata.presentation, &*metadata.fileProperties), "re-save full refined native metadata");
        exact([&] { return results(item(destination).Get(), fixture); }, example.after, stage + "-replacement-native-saved");
        explorer::SavedSearchMetadata restored; checked(explorer::readSavedSearch(destination, &restored), "reimport refined saved query");
        require(restored.fileProperties && *restored.fileProperties == properties && restored.presentation &&
            restored.presentation->mode == presentation.mode && restored.presentation->visibleColumns == presentation.visibleColumns &&
            restored.presentation->sort && restored.presentation->sort->size() == 1 && restored.presentation->sort->front().direction == SORT_DESCENDING &&
            restored.scopeRules.size() == 1 && !restored.scopeRules.front().recursive && !restored.scopeRules.front().excluded,
            "refinement lost exact file metadata, view or recursion rules");
        int same = 1; checked(restored.scope->Compare(scope.Get(), SICHINT_CANONICAL, &same), "compare unchanged native refinement scope"); require(same == 0, "refinement changed scope identity");
        std::shared_ptr<NativeSearchRefinements> current; checked(NativeSearchRefinements::inspect(restored.query, &current), "inspect final full native predicate");
        bool selected = false; checked(current->matches(example.category, example.preset, &selected), "match actual complete selected category"); require(selected, "current full predicate did not select exact preset");
        std::wstring noOp; checked(current->replace(example.category, example.preset, &noOp), "same-preset no-op"); require(noOp == restored.query, "same preset changed original full query text");
        require(identity(source) == originalSourceIdentity && bytes(source) == originalSourceBytes,
                "import/refinement changed original native saved query bytes or FileID");
    }
    fixture.unchanged();
}
void rejectionAndAbsoluteBase() {
    constexpr auto unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const std::wstring today = L"System.DateModified:System.StructuredQueryType.DateTime#Today";
    for (const auto& query : {L"(" + today + L" OR System.Size:>0)", L"NOT (" + today + L" AND System.FileName:=\"owned\")"}) {
        std::shared_ptr<NativeSearchRefinements> model; checked(NativeSearchRefinements::inspect(query, &model), "inspect entangled native Boolean predicate");
        std::wstring output = L"untouched";
        require(model->replace(Category::Date, L"System.DateModified:System.StructuredQueryType.DateTime#LastMonth", &output) == unsupported && output == L"untouched",
                "entangled Date rewrite changed output or discarded a Boolean branch");
        bool selected = true; checked(model->matches(Category::Date, today, &selected), "inspect ambiguous preset state"); require(!selected, "entangled predicate falsely selected a single preset");
    }
    std::shared_ptr<NativeSearchRefinements> model; checked(NativeSearchRefinements::inspect(today, &model), "inspect valid failure-preservation base");
    const auto original = model; require(NativeSearchRefinements::inspect(std::wstring(L"a\0b", 3), &model) == E_INVALIDARG && model == original, "failed inspection replaced valid native snapshot");
    std::wstring output = L"untouched"; require(model->replace(Category::Date, L"System.Kind:=System.Kind#Folder", &output) == E_INVALIDARG && output == L"untouched", "wrong-property preset mutated output");
    std::shared_ptr<NativeSearchRefinements> multi;
    checked(NativeSearchRefinements::inspect(L"(" + today + L" OR System.DateModified:System.StructuredQueryType.DateTime#LastMonth)", &multi), "inspect category-only OR");
    bool selected = true; checked(multi->matches(Category::Date, today, &selected), "read multi-preset selection"); require(!selected, "multi-date category falsely matched Today");
    checked(multi->replace(Category::Date, today, &output), "replace entire category-only OR");
    Fixture fixture; const auto scope = item(fixture.scope);
    const std::wstring absolute = L"System.FileName:~\"target-*.txt\" AND System.DateCreated:>=2024-01-01T00:00:00Z AND System.Size:>0";
    checked(NativeSearchRefinements::inspect(absolute, &model), "inspect absolute DateTime base");
    checked(model->replace(Category::Size, L"System.Size:System.Size#Empty", &output), "retain absolute DateTime while replacing Size");
    exact([&] { return results(live(output, scope.Get()).Get(), fixture); }, {fixture.identities[2]}, "absolute-DateCreated-retained-Size-replacement");
    const std::wstring compoundBase = L"NOT System.FileName:=\"Folder\" AND NOT System.FileName:=\"different-month.txt\" AND System.Kind:=System.Kind#Document";
    const std::wstring compoundPreset = L"System.Kind:=System.Kind#Document OR System.Kind:=System.Kind#Folder";
    const std::set<Identity> compoundExpected{fixture.identities[0], fixture.identities[1], fixture.identities[2]};
    exact([&] { return results(live(compoundBase, scope.Get()).Get(), fixture); }, compoundExpected, "compound-Kind-original");
    checked(NativeSearchRefinements::inspect(compoundBase, &model), "inspect retained NOT factors before compound Kind replacement");
    checked(model->replace(Category::Kind, compoundPreset, &output), "replace Kind with grouped category-only native OR");
    exact([&] { return results(live(output, scope.Get()).Get(), fixture); }, compoundExpected, "compound-Kind-replacement-retains-NOT");
    checked(NativeSearchRefinements::inspect(output, &model), "inspect complete compound Kind output");
    selected = false; checked(model->matches(Category::Kind, compoundPreset, &selected), "match complete category-only Kind OR");
    require(selected, "complete compound Kind predicate did not match its native preset");
    selected = true; checked(model->matches(Category::Kind, L"System.Kind:=System.Kind#Document", &selected), "reject single Kind selected state for a category union");
    require(!selected, "compound Kind predicate falsely selected a single Kind row");
    std::shared_ptr<NativeSearchRefinements> empty; checked(NativeSearchRefinements::inspect(L"", &empty), "inspect empty initial query");
    checked(empty->replace(Category::Size, L"System.Size:System.Size#Empty", &output), "create category-only initial search");
    checked(NativeSearchRefinements::inspect(output, &model), "inspect real category-only output");
    selected = false; checked(model->matches(Category::Size, L"System.Size:System.Size#Empty", &selected), "match initial native category");
    require(selected, "initial category-only query lost its native condition");
    fixture.unchanged();
}
void installedKindPresets() {
    ComPtr<IPropertyDescription> description; ComPtr<IPropertyEnumTypeList> types;
    checked(PSGetPropertyDescription(PKEY_Kind, IID_PPV_ARGS(&description)), "read actual installed Kind description");
    checked(description->GetEnumTypeList(IID_PPV_ARGS(&types)), "read actual installed Kind enum list");
    UINT count = 0; checked(types->GetCount(&count), "read native Kind enum count");
    require(count && count <= 64, "actual native Kind enum inventory missing or unsupported");
    std::vector<std::wstring> values;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IPropertyEnumType> type; checked(types->GetAt(index, IID_PPV_ARGS(&type)), "read native Kind enum entry");
        PROPVARIANT value{}; const auto hr = type->GetValue(&value);
        const bool valid = SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal && *value.pwszVal;
        if (valid) values.emplace_back(value.pwszVal);
        PropVariantClear(&value); checked(hr, "read native Kind enum value"); require(valid, "native Kind enum is not a complete string value");
    }
    ComPtr<IQueryParserManager> manager; ComPtr<IQueryParser> parser;
    checked(CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)), "independent native Kind parser manager");
    checked(manager->CreateLoadedParser(L"SystemIndex", GetUserDefaultUILanguage(), IID_PPV_ARGS(&parser)), "independent native Kind parser");
    checked(manager->InitializeOptions(FALSE, TRUE, parser.Get()), "independent native Kind parser options");
    std::vector<std::wstring> expressions;
    const auto derive = NativeSearchRefinements::kindPresets(values, &expressions);
    if (FAILED(derive)) {
        for (size_t index = 0; index < values.size(); ++index) {
            std::vector<std::wstring> single;
            const auto individual = NativeSearchRefinements::kindPresets({values[index]}, &single);
            if (SUCCEEDED(individual)) continue;
            std::cerr << "Native Kind derivation failed: enumIndex=" << index << " valueUnits=" << values[index].size() << " HRESULT=0x" << std::hex << static_cast<unsigned long>(individual) << std::dec << '\n';
            ComPtr<IConditionFactory> factory; ComPtr<ICondition> expected;
            checked(CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Kind diagnostic factory");
            PROPVARIANT nativeValue{}; checked(InitPropVariantFromString(values[index].c_str(), &nativeValue), "Kind diagnostic enum value");
            const auto make = factory->MakeLeaf(L"System.Kind", COP_EQUAL, L"System.StructuredQueryType.String", &nativeValue, nullptr, nullptr, nullptr, FALSE, &expected);
            PropVariantClear(&nativeValue); checked(make, "Kind diagnostic original native leaf");
            PWSTR raw = nullptr; checked(parser->RestateToString(expected.Get(), FALSE, &raw), "Kind diagnostic native restatement");
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> restated(raw, &CoTaskMemFree);
            require(raw && *raw, "Kind diagnostic restatement missing");
            std::cerr << "Native Kind restated static enum query=" << diagnosticAscii(raw) << '\n';
            ComPtr<IQuerySolution> context; ComPtr<ICondition> actual, expectedResolved, actualResolved;
            checked(parser->Parse(raw, nullptr, &context), "Kind diagnostic reparse"); checked(context->GetQuery(&actual, nullptr), "Kind diagnostic parsed tree");
            SYSTEMTIME now{}; GetLocalTime(&now);
            checked(context->Resolve(expected.Get(), SQRO_DONT_SPLIT_WORDS, &now, &expectedResolved), "Kind diagnostic expected Resolve");
            checked(context->Resolve(actual.Get(), SQRO_DONT_SPLIT_WORDS, &now, &actualResolved), "Kind diagnostic actual Resolve");
            const auto describe = [&](const char* label, ICondition* node) {
                CONDITION_TYPE type{}; checked(node->GetConditionType(&type), "Kind diagnostic native type");
                std::cerr << label << " conditionType=" << type;
                if (type == CT_LEAF_CONDITION) {
                    PWSTR property = nullptr, semantic = nullptr; CONDITION_OPERATION operation{}; PROPVARIANT value{};
                    checked(node->GetComparisonInfo(&property, &operation, &value), "Kind diagnostic leaf fields");
                    checked(node->GetValueType(&semantic), "Kind diagnostic semantic type");
                    std::cerr << " property=" << diagnosticAscii(property) <<
                        " operation=" << operation << " vartype=" << value.vt << " semantic=" <<
                        diagnosticAscii(semantic) << " valueUnits=" <<
                        (value.vt == VT_LPWSTR && value.pwszVal ? wcslen(value.pwszVal) : 0);
                    if (value.vt == VT_LPWSTR && value.pwszVal) std::cerr << " staticValue=" << diagnosticAscii(value.pwszVal);
                    CoTaskMemFree(property); CoTaskMemFree(semantic); PropVariantClear(&value);
                }
                std::cerr << '\n';
            };
            describe("expectedRaw", expected.Get()); describe("actualParsed", actual.Get());
            describe("expectedResolved", expectedResolved.Get()); describe("actualResolved", actualResolved.Get());
            break;
        }
    }
    checked(derive, "derive all native Kind expressions with complete equivalence");
    require(expressions.size() == values.size(), "native Kind rows were truncated or shifted");
    for (size_t index = 0; index < expressions.size(); ++index) {
        ComPtr<IQuerySolution> solution; ComPtr<ICondition> condition, resolved;
        checked(parser->Parse(expressions[index].c_str(), nullptr, &solution), "independently parse native enum expression");
        checked(solution->GetQuery(&condition, nullptr), "read complete native enum predicate");
        SYSTEMTIME now{}; GetLocalTime(&now);
        checked(solution->Resolve(condition.Get(), SQRO_DONT_SPLIT_WORDS, &now, &resolved), "independently resolve native enum expression");
        CONDITION_TYPE type{}; checked(resolved->GetConditionType(&type), "read resolved Kind connective");
        require(type == CT_LEAF_CONDITION, "native enum query became a generic compound");
        PWSTR property = nullptr; CONDITION_OPERATION operation{}; PROPVARIANT value{};
        const auto hr = resolved->GetComparisonInfo(&property, &operation, &value);
        const bool exact = SUCCEEDED(hr) && property && _wcsicmp(property, L"System.Kind") == 0 &&
            operation == COP_EQUAL && value.vt == VT_LPWSTR && value.pwszVal && values[index] == value.pwszVal;
        CoTaskMemFree(property); PropVariantClear(&value); checked(hr, "read actual native enum leaf fields");
        require(exact, "native enum query changed canonical property, operation, VARTYPE or actual enum value");
    }
    std::cout << "Native Kind enums: providerCount=" << count << " completeValidatedExpressions=" << expressions.size() << '\n';
    Fixture fixture; const auto scope = item(fixture.scope);
    const auto document = std::find(values.begin(), values.end(), L"document"), folder = std::find(values.begin(), values.end(), L"folder");
    require(document != values.end() && folder != values.end(), "native Document/Folder rows missing");
    const auto documentIndex = static_cast<size_t>(document - values.begin()), folderIndex = static_cast<size_t>(folder - values.begin());
    exact([&] { return results(live(expressions[documentIndex], scope.Get()).Get(), fixture); },
          {fixture.identities[0], fixture.identities[1], fixture.identities[2], fixture.identities[3]}, "actual-native-Document-row");
    std::shared_ptr<NativeSearchRefinements> model; checked(NativeSearchRefinements::inspect(expressions[documentIndex], &model), "inspect actual native Document row");
    std::wstring output; checked(model->replace(Category::Kind, expressions[folderIndex], &output), "replace actual native Document row with Folder row");
    exact([&] { return results(live(output, scope.Get()).Get(), fixture); }, {fixture.identities[4]}, "actual-native-Folder-row-replacement");
    const auto searchFolder = std::find(values.begin(), values.end(), L"searchfolder");
    require(searchFolder != values.end(), "native SearchFolder enum row missing");
    const auto searchFolderIndex = static_cast<size_t>(searchFolder - values.begin());
    const auto saved = fixture.scope / L"Owned Kind query.search-ms";
    checked(explorer::saveSearch(L"System.FileName:~\"target-*.txt\"", scope.Get(), false, saved), "create actual owned SearchFolder Kind member");
    const auto savedIdentity = identity(saved); const auto savedBytes = bytes(saved);
    exact([&] { return results(live(expressions[searchFolderIndex], scope.Get()).Get(), fixture); },
          {savedIdentity}, "actual-native-SearchFolder-row");
    checked(model->replace(Category::Kind, expressions[searchFolderIndex], &output), "replace native Document row with verified SearchFolder row");
    exact([&] { return results(live(output, scope.Get()).Get(), fixture); }, {savedIdentity}, "actual-native-SearchFolder-row-replacement");
    require(identity(saved) == savedIdentity && bytes(saved) == savedBytes, "Kind replacement changed the actual saved query member");
    fixture.unchanged();
}
}
int runSearchRefinementTests() {
    const std::array<std::pair<const char*, std::function<void()>>, 3> tests{{
        {"native typed/imported Date/Kind/Size replacement and complete metadata/FileID round trips", replacementRoutes},
        {"native entangled-category rejection, absolute-date retention and atomic outputs", rejectionAndAbsoluteBase},
        {"all installed Kind enum expressions retain exact native fields and actual Document/Folder membership", installedKindPresets}
    }};
    int failures = 0;
    for (const auto& [name, test] : tests) { try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << name << ": " << error.what() << '\n'; } }
    return failures;
}
