#include "explorer/search.hpp"
#include "explorer/saved_search.hpp"
#include "explorer/search_presentation_store.hpp"
#include "../src/search_scope_internal.hpp"

#include <shlobj.h>
#include <shlguid.h>
#include <propkey.h>
#include <propvarutil.h>
#include <structuredquery.h>
#include <sddl.h>
#include <aclapi.h>
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
#include <cstring>
#include <tuple>

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

struct NativeQuerySignature {
    std::wstring tree, normalizedTree;
    CONDITION_TYPE root = CT_LEAF_CONDITION, normalizedRoot = CT_LEAF_CONDITION;
    unsigned nodes = 0, literalLeaves = 0;
    unsigned normalizedNodes = 0, normalizedLiteralLeaves = 0;
};
NativeQuerySignature resolvedLiteralSignature(const std::wstring& query, const std::wstring& literal,
                                             const SYSTEMTIME& reference, ICondition* nativeCondition = nullptr,
                                             bool requireExactLiteral = true) {
    ComPtr<IQueryParserManager> manager;
    succeeded(CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&manager)), "create independent native condition parser");
    ComPtr<IQueryParser> parser;
    succeeded(manager->CreateLoadedParser(L"SystemIndex", GetUserDefaultUILanguage(), IID_PPV_ARGS(&parser)),
              "load canonical native query schema");
    succeeded(manager->InitializeOptions(FALSE, TRUE, parser.Get()), "initialize native query generators");
    const std::array<std::pair<const wchar_t*, const wchar_t*>, 5> defaults{{
        {L"System.StructuredQueryType.String", L"System.Generic.String"},
        {L"System.StructuredQueryType.Integer", L"System.Generic.Integer"},
        {L"System.StructuredQueryType.DateTime", L"System.Generic.DateTime"},
        {L"System.StructuredQueryType.Boolean", L"System.Generic.Boolean"},
        {L"System.StructuredQueryType.FloatingPoint", L"System.Generic.FloatingPoint"}
    }};
    for (const auto& [type, property] : defaults) {
        PROPVARIANT value{};
        succeeded(InitPropVariantFromString(property, &value), "make native default property option");
        const auto status = parser->SetMultiOption(SQMO_DEFAULT_PROPERTY, type, &value);
        PropVariantClear(&value);
        succeeded(status, "retain native generic query defaults");
    }
    ComPtr<IQuerySolution> solution;
    ComPtr<ICondition> parsed, resolved;
    if (nativeCondition) resolved = nativeCondition;
    else {
        succeeded(parser->Parse(query.c_str(), nullptr, &solution), "parse original or imported literal query");
        succeeded(solution->GetQuery(&parsed, nullptr), "read complete native literal condition");
        succeeded(solution->Resolve(parsed.Get(), SQRO_DONT_SPLIT_WORDS, &reference, &resolved),
                  "resolve complete native condition at the same reference time");
    }
    require(resolved != nullptr, "native literal resolution returned no condition");
    NativeQuerySignature result;
    succeeded(resolved->GetConditionType(&result.root), "read resolved native root type");
    const auto field = [](std::wstring& target, std::wstring_view text) {
        target += std::to_wstring(text.size()) + L":"; target += text;
    };
    std::function<std::wstring(ICondition*, unsigned)> visit;
    visit = [&](ICondition* condition, unsigned depth) -> std::wstring {
        require(condition && depth <= 64 && ++result.nodes <= 128, "literal condition tree exceeded fixture bounds");
        CONDITION_TYPE type{};
        succeeded(condition->GetConditionType(&type), "read every native condition type");
        std::wstring signature = std::to_wstring(type) + L"{";
        if (type == CT_LEAF_CONDITION) {
            struct Comparison {
                PWSTR property = nullptr, semantic = nullptr;
                PROPVARIANT value{};
                ~Comparison() { CoTaskMemFree(property); CoTaskMemFree(semantic); PropVariantClear(&value); }
            } comparison;
            CONDITION_OPERATION operation{};
            succeeded(condition->GetComparisonInfo(&comparison.property, &operation, &comparison.value),
                      "read exact native leaf property, operation and typed value");
            succeeded(condition->GetValueType(&comparison.semantic), "read exact native leaf semantic type");
            require(comparison.property && *comparison.property, "resolved literal lost its canonical native property");
            // This fixture's complete Title expansion must retain every copy
            // of the original string. A changed VARTYPE, omitted branch or
            // partial phrase fails instead of becoming a textual equivalent.
            require(comparison.value.vt == VT_LPWSTR && comparison.value.pwszVal,
                    "native Title expansion changed its exact value type");
            const bool exactLiteral = std::wstring_view(comparison.value.pwszVal) == literal;
            if (requireExactLiteral) require(exactLiteral, "native Title expansion changed an exact Unicode literal occurrence");
            if (exactLiteral) ++result.literalLeaves;
            field(signature, comparison.property);
            signature += comparison.semantic ? L"1:" : L"0:";
            field(signature, comparison.semantic ? comparison.semantic : L"");
            signature += std::to_wstring(operation) + L":" + std::to_wstring(comparison.value.vt) + L":";
            field(signature, comparison.value.pwszVal);
        } else {
            std::vector<std::wstring> children;
            if (type == CT_NOT_CONDITION) {
                ComPtr<ICondition> child;
                succeeded(condition->GetSubConditions(IID_PPV_ARGS(&child)), "read native negated condition");
                children.push_back(visit(child.Get(), depth + 1));
            } else {
                require(type == CT_AND_CONDITION || type == CT_OR_CONDITION, "unknown native connective in Title expansion");
                ComPtr<IEnumUnknown> enumeration;
                succeeded(condition->GetSubConditions(IID_PPV_ARGS(&enumeration)), "read every native compound branch");
                for (;;) {
                    ComPtr<IUnknown> unknown;
                    const auto status = enumeration->Next(1, &unknown, nullptr);
                    if (status == S_FALSE) break;
                    succeeded(status, "enumerate complete native Title expansion");
                    require(unknown != nullptr, "native compound branch has no identity");
                    ComPtr<ICondition> child;
                    succeeded(unknown.As(&child), "read actual native compound child");
                    children.push_back(visit(child.Get(), depth + 1));
                }
                // AND/OR branch order is immaterial; connective type, nesting
                // and every duplicate branch remain in the exact signature.
                std::sort(children.begin(), children.end());
            }
            require(!children.empty(), "native literal compound lost all branches");
            signature += std::to_wstring(children.size()) + L":";
            for (const auto& child : children) field(signature, child);
        }
        signature += L"}"; return signature;
    };
    result.tree = visit(resolved.Get(), 0);
    if (requireExactLiteral) require(result.literalLeaves != 0, "native literal condition has no exact literal leaves");
    auto raw = std::move(result);
    ComPtr<IConditionFactory> factory;
    succeeded(CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
              "create independent native Boolean simplifier");
    ComPtr<ICondition> normalized;
    if (raw.root == CT_AND_CONDITION || raw.root == CT_OR_CONDITION) {
        ComPtr<IEnumUnknown> children;
        succeeded(resolved->GetSubConditions(IID_PPV_ARGS(&children)), "retain every actual branch for native simplification");
        succeeded(factory->MakeAndOr(raw.root, children.Get(), TRUE, &normalized),
                  "ask Windows to simplify the complete native Boolean condition");
    } else if (raw.root == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        succeeded(resolved->GetSubConditions(IID_PPV_ARGS(&child)), "retain actual negated condition for simplification");
        succeeded(factory->MakeNot(child.Get(), TRUE, &normalized), "let Windows simplify actual native negation");
    } else normalized = resolved;
    require(normalized != nullptr, "native condition simplifier returned no condition");
    result = {};
    // The installed native factory retains the parser's repeated OR leaves
    // even with fSimplify=TRUE. Apply only Boolean associativity, idempotence
    // and singleton identity to complete child signatures. No leaf field is
    // discarded, and different connectives and NOT boundaries remain distinct.
    struct BooleanSignature {
        std::wstring text;
        CONDITION_TYPE type = CT_LEAF_CONDITION;
        unsigned nodes = 1, literalLeaves = 0;
    };
    unsigned normalizedVisits = 0;
    std::function<BooleanSignature(ICondition*, unsigned)> booleanSignature;
    booleanSignature = [&](ICondition* condition, unsigned depth) -> BooleanSignature {
        require(condition && depth <= 64 && ++normalizedVisits <= 128, "normalized condition exceeded fixture bounds");
        BooleanSignature signature;
        succeeded(condition->GetConditionType(&signature.type), "retain each Boolean condition's actual connective");
        if (signature.type == CT_LEAF_CONDITION) {
            const auto before = result.literalLeaves;
            signature.text = visit(condition, depth);
            signature.literalLeaves = result.literalLeaves - before;
            return signature;
        }
        std::vector<BooleanSignature> children;
        if (signature.type == CT_NOT_CONDITION) {
            ComPtr<ICondition> child;
            succeeded(condition->GetSubConditions(IID_PPV_ARGS(&child)), "retain complete normalized NOT boundary");
            children.push_back(booleanSignature(child.Get(), depth + 1));
        } else {
            require(signature.type == CT_AND_CONDITION || signature.type == CT_OR_CONDITION,
                    "unknown connective during native Boolean comparison");
            std::function<void(ICondition*, unsigned)> collect;
            collect = [&](ICondition* child, unsigned childDepth) {
                require(child && childDepth <= 64, "associative native condition exceeded fixture depth");
                CONDITION_TYPE childType{};
                succeeded(child->GetConditionType(&childType), "read actual child connective before associative flattening");
                if (childType != signature.type) { children.push_back(booleanSignature(child, childDepth)); return; }
                require(++normalizedVisits <= 128, "associative native condition exceeded fixture node bound");
                ComPtr<IEnumUnknown> enumeration;
                succeeded(child->GetSubConditions(IID_PPV_ARGS(&enumeration)), "retain every same-connective native branch");
                for (;;) {
                    ComPtr<IUnknown> unknown;
                    const auto status = enumeration->Next(1, &unknown, nullptr);
                    if (status == S_FALSE) break;
                    succeeded(status, "read complete associative native branch");
                    require(unknown != nullptr, "associative native branch has no identity");
                    ComPtr<ICondition> nested;
                    succeeded(unknown.As(&nested), "read actual same-connective native child");
                    collect(nested.Get(), childDepth + 1);
                }
            };
            collect(condition, depth);
            std::sort(children.begin(), children.end(), [](const auto& first, const auto& second) { return first.text < second.text; });
            children.erase(std::unique(children.begin(), children.end(), [](const auto& first, const auto& second) {
                return first.text == second.text;
            }), children.end());
            if (children.size() == 1) return std::move(children.front());
        }
        signature.text = std::to_wstring(signature.type) + L"{" + std::to_wstring(children.size()) + L":";
        for (const auto& child : children) {
            field(signature.text, child.text); signature.nodes += child.nodes; signature.literalLeaves += child.literalLeaves;
        }
        signature.text += L"}"; return signature;
    };
    const auto semantic = booleanSignature(normalized.Get(), 0);
    raw.normalizedRoot = semantic.type; raw.normalizedTree = semantic.text;
    raw.normalizedNodes = semantic.nodes; raw.normalizedLiteralLeaves = semantic.literalLeaves;
    return raw;
}

void nativeLiteralComparatorCounterexamples(const std::wstring& query, const std::wstring& literal,
                                          const SYSTEMTIME& reference, const NativeQuerySignature& expected) {
    ComPtr<IConditionFactory2> factory;
    succeeded(CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
              "create real native counterfeit-condition factory");
    const auto leaf = [&](const wchar_t* property, CONDITION_OPERATION operation, const std::wstring& text) {
        PROPVARIANT value{};
        succeeded(InitPropVariantFromString(text.c_str(), &value), "create actual native comparison value");
        ComPtr<ICondition> result;
        const auto status = factory->MakeLeaf(property, operation, L"System.StructuredQueryType.String", &value,
                                              nullptr, nullptr, nullptr, FALSE, &result);
        PropVariantClear(&value); succeeded(status, "create actual native comparison leaf"); return result;
    };
    const auto compound = [&](CONDITION_TYPE type, std::initializer_list<ICondition*> children) {
        std::vector<ICondition*> items(children);
        ComPtr<ICondition> result;
        succeeded(factory->CreateCompoundFromArray(type, items.data(), static_cast<ULONG>(items.size()),
                                                   CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&result)),
                  "create actual native structured Boolean control"); return result;
    };
    const auto signature = [&](ICondition* condition) {
        return resolvedLiteralSignature(query, literal, reference, condition, false).normalizedTree;
    };
    const auto original = leaf(L"System.Title", COP_EQUAL, literal);
    require(signature(original.Get()) == expected.normalizedTree, "normalized original Title changed exact native leaf meaning");
    const auto repeated = compound(CT_OR_CONDITION, {original.Get(), original.Get(), original.Get()});
    const auto nested = compound(CT_OR_CONDITION, {original.Get(), repeated.Get()});
    const auto singleton = compound(CT_OR_CONDITION, {original.Get()});
    require(signature(repeated.Get()) == expected.normalizedTree && signature(nested.Get()) == expected.normalizedTree &&
            signature(singleton.Get()) == expected.normalizedTree,
            "native Boolean normalization did not preserve exact associative/idempotent/singleton meaning");
    const auto changedValue = leaf(L"System.Title", COP_EQUAL, literal + L" altered");
    const auto changedOperation = leaf(L"System.Title", COP_NOTEQUAL, literal);
    const auto changedProperty = leaf(L"System.Subject", COP_EQUAL, literal);
    ComPtr<ICondition> negated;
    succeeded(factory->MakeNot(original.Get(), FALSE, &negated), "construct actual native NOT counterexample");
    for (const auto changed : {changedValue.Get(), changedOperation.Get(), changedProperty.Get(), negated.Get()})
        require(signature(changed) != expected.normalizedTree,
                "complete native condition comparator ignored a changed value, operation, property or NOT");
    const auto conjunction = compound(CT_AND_CONDITION, {original.Get(), changedProperty.Get()});
    const auto disjunction = compound(CT_OR_CONDITION, {original.Get(), changedProperty.Get()});
    require(signature(conjunction.Get()) != signature(disjunction.Get()),
            "complete native condition comparator confused AND and OR with identical actual leaf fields");
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
                            const char* message,
                            const std::function<void(const std::set<std::wstring>&)>& failureDiagnostic = {}) {
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
            if (failureDiagnostic) {
                std::cerr << "INFO: confirmed replacement failure retries=" << retries
                          << " elapsed_ms=" << GetTickCount64() - started << '\n';
                // Observe only after the original bounded result check has
                // failed. Keep its captured membership and strict assertion,
                // even if a diagnostic provider call itself fails.
                try { failureDiagnostic(actual); }
                catch (const std::exception& error) {
                    std::cerr << "INFO: confirmed replacement diagnostic failed: " << error.what() << '\n';
                }
                catch (...) { std::cerr << "INFO: confirmed replacement diagnostic failed with unknown exception\n"; }
            }
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
    const std::wstring literal = L"猫 & \"quoted\" < > ' \U0001F680";
    const std::wstring query = L"System.Title:=\"猫 & \"\"quoted\"\" < > ' \U0001F680\"";
    const auto path = fixture.root / L"escaped.search-ms";
    succeeded(explorer::saveSearch(query, scope.Get(), true, path), "save escaped Unicode query");
    const auto bytes = read(path);
    for (const auto* escape : {"&amp;", "&quot;", "&lt;", "&gt;", "&apos;"})
        require(bytes.find(escape) != std::string::npos, "XML metacharacter was not escaped");
    const auto document = loadXml(path);
    const auto condition = xmlElement(document.Get(), L"/persistedQuery/query/conditions/condition");
    const auto rootType = xmlAttribute(condition.Get(), L"type");
    XmlString leafExpression(L"/persistedQuery/query/conditions//condition[@type='leafCondition']");
    ComPtr<IXMLDOMNodeList> leaves;
    succeeded(document->selectNodes(leafExpression.value, &leaves), "read every emitted Title value leaf");
    require(leaves != nullptr, "saved Title query has no condition collection");
    long leafCount = 0;
    succeeded(leaves->get_length(&leafCount), "count all emitted literal occurrences");
    std::cout << "INFO: escaped Title XML root=" << utf8(rootType) << " literalLeaves=" << leafCount
              << " literalUtf16Units=" << literal.size() << '\n';
    require(leafCount > 0, "saved Title query omitted every literal value leaf");
    for (long index = 0; index < leafCount; ++index) {
        ComPtr<IXMLDOMNode> node;
        succeeded(leaves->get_item(index, &node), "read each emitted literal node");
        ComPtr<IXMLDOMElement> leaf;
        succeeded(node.As(&leaf), "read emitted native Title leaf attributes");
        require(xmlAttribute(leaf.Get(), L"value") == literal, "XML escaping changed a literal Unicode occurrence");
    }
    explorer::SavedSearchMetadata imported;
    succeeded(explorer::readSavedSearch(path, &imported), "import exact escaped Title query semantics");
    SYSTEMTIME reference{}; reference.wYear = 2024; reference.wMonth = 8; reference.wDay = 19; reference.wHour = 12;
    const auto expected = resolvedLiteralSignature(query, literal, reference);
    const auto actual = resolvedLiteralSignature(imported.query, literal, reference);
    std::cout << "INFO: escaped Title comparison originalRoot=" << static_cast<unsigned>(expected.root)
              << " importedRoot=" << static_cast<unsigned>(actual.root) << " originalNodes=" << expected.nodes
              << " importedNodes=" << actual.nodes << " originalLeaves=" << expected.literalLeaves
              << " importedLeaves=" << actual.literalLeaves << " completeSignatureEqual=" << (actual.tree == expected.tree) << '\n';
    std::cout << "INFO: escaped Title normalized originalRoot=" << static_cast<unsigned>(expected.normalizedRoot)
              << " importedRoot=" << static_cast<unsigned>(actual.normalizedRoot) << " originalNodes=" << expected.normalizedNodes
              << " importedNodes=" << actual.normalizedNodes << " completeSignatureEqual="
              << (actual.normalizedTree == expected.normalizedTree) << '\n';
    require(actual.normalizedTree == expected.normalizedTree && actual.normalizedNodes == expected.normalizedNodes &&
            actual.normalizedLiteralLeaves == expected.normalizedLiteralLeaves && expected.literalLeaves == static_cast<unsigned>(leafCount),
            "saved Title query changed its complete native connective/property/operation/type/value tree");
    nativeLiteralComparatorCounterexamples(query, literal, reference, expected);
    std::cout << "INFO: escaped Title native root=" << static_cast<unsigned>(expected.root)
              << " nodes=" << expected.nodes << " exactLiteralOccurrences=" << expected.literalLeaves << '\n';
    const auto saved = reopenSearch(path);
    require(searchResults(saved.Get()).empty(), "unmatched literal query was broadened while saving");

    const auto computerPath = fixture.root / L"this-pc.search-ms";
    succeeded(explorer::saveSearch(L"System.FileName:=\"unique-no-results-7EB8D806\"", nullptr, true, computerPath), "save This PC query");
    const auto computerDocument = loadXml(computerPath);
    const auto include = xmlElement(computerDocument.Get(), L"/persistedQuery/query/scope/include");
    wchar_t identifier[40]{};
    require(StringFromGUID2(FOLDERID_ComputerFolder, identifier, 40) != 0, "format This PC known-folder id");
    require(xmlAttribute(include.Get(), L"knownFolder") == identifier, "saved This PC scope used a filesystem substitute");
    explorer::SavedSearchMetadata computer;
    succeeded(explorer::readSavedSearch(computerPath, &computer), "import native This PC saved-search scope");
    require(computer.scope != nullptr, "saved This PC metadata omitted its native scope");
    ComPtr<IShellItem> computerFolder;
    succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&computerFolder)),
              "retain actual native This PC scope identity");
    int scopeComparison = 1;
    succeeded(computer.scope->Compare(computerFolder.Get(), SICHINT_CANONICAL, &scopeComparison),
              "compare saved native This PC scope without userwide enumeration");
    require(scopeComparison == 0 && computer.recursive, "saved This PC metadata names a different native folder");
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
    require(!fs::exists(rejected), "rejected query left a saved-search file");
    write(fixture.root / L"ordinary-file.txt", "not a folder");
    auto file = shellItem(fixture.root / L"ordinary-file.txt");
    require(explorer::saveSearch(L"System.FileName:=\"report\"", file.Get(), true, rejected) == HRESULT_FROM_WIN32(ERROR_DIRECTORY), "file used as search scope");
    ComPtr<IShellItem> result;
    require(explorer::createSearchFolder(L"report", nullptr, &result, false) == unsupported && !result, "shallow virtual scope was silently recursive");
    require(explorer::saveSearch(L"System.FileName:=\"report\"", nullptr, false, rejected) == unsupported, "saved shallow virtual scope was silently recursive");
    require(explorer::createSearchFolder(L"report", scope.Get(), nullptr) == E_POINTER, "null search output accepted");
    require(explorer::createSearchFolder(L"", scope.Get(), &result) == E_INVALIDARG && !result, "invalid live search retained result");
    // Native filename word-prefix membership and persistence are covered by
    // the owned saved-metadata fixture; all nested parsed forms remain valid.
    for (const auto* query : {
        L"System.FileName:$<\"match\"", L"System.FILENAME:$<\"match\"",
        L"System.Kind:=System.Kind#Document AND System.FileName:$<\"match\"",
        L"System.FileName:$<\"match\" OR System.Size:>1",
        L"NOT System.FileName:$<\"match\""}) {
        result.Reset();
        succeeded(explorer::createSearchFolder(query, scope.Get(), &result), "construct explicit native filename word-prefix query");
        require(result != nullptr, "valid filename prefix has no native identity");
    }
    for (const auto* query : {L"\"literal $< text\"", L"System.FileName:\"literal $< text\"",
                              L"System.Search.Contents:$<\"match\"", L"ordinary default term"}) {
        result.Reset();
        succeeded(explorer::createSearchFolder(query, scope.Get(), &result),
                  "native search rejected unrelated property or literal/default text");
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
struct DaclState {
    std::vector<BYTE> descriptor;
    std::wstring sddl;
    SECURITY_DESCRIPTOR_CONTROL control{};
};
DaclState fileDaclState(const fs::path& path) {
    DWORD length = 0;
    GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &length);
    require(length != 0, "read owned saved-search DACL size");
    DaclState result; result.descriptor.resize(length);
    require(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, result.descriptor.data(), length, &length) != FALSE,
            "read owned saved-search DACL");
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(result.descriptor.data(), &result.control, &revision) != FALSE,
            "read owned saved-search DACL control");
    PWSTR raw = nullptr;
    require(ConvertSecurityDescriptorToStringSecurityDescriptorW(result.descriptor.data(), SDDL_REVISION_1,
        DACL_SECURITY_INFORMATION, &raw, nullptr) != FALSE && raw, "format owned saved-search DACL");
    result.sddl = raw;
    LocalFree(raw);
    return result;
}
std::wstring fileDacl(const fs::path& path) { return fileDaclState(path).sddl; }
void describeDacl(const char* phase, const DaclState& state) {
    PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    require(GetSecurityDescriptorDacl(const_cast<BYTE*>(state.descriptor.data()), &present, &acl, &defaulted) != FALSE,
            "decode owned DACL diagnostic");
    // Never print SDDL, trustees, hashes of SIDs, paths or user identities.
    // All the reported facts concern exclusively owned fixture descriptors.
    std::cerr << "owned saved-search DACL " << phase << " control=0x" << std::hex << state.control << std::dec
              << " present=" << (present != FALSE) << " null=" << (acl == nullptr) << " defaulted=" << (defaulted != FALSE)
              << " revision=" << (acl ? acl->AclRevision : 0) << " aceCount=" << (acl ? acl->AceCount : 0) << '\n';
    for (DWORD index = 0; acl && index < acl->AceCount; ++index) {
        void* raw = nullptr; require(GetAce(acl, index, &raw) != FALSE, "decode owned diagnostic ACE");
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        ACCESS_MASK mask = 0;
        if (header->AceSize >= sizeof(ACE_HEADER) + sizeof(mask))
            std::memcpy(&mask, static_cast<const BYTE*>(raw) + sizeof(ACE_HEADER), sizeof(mask));
        std::cerr << "  aceIndex=" << index << " type=" << static_cast<unsigned>(header->AceType)
                  << " flags=0x" << std::hex << static_cast<unsigned>(header->AceFlags) << " mask=0x" << mask
                  << std::dec << " bytes=" << header->AceSize << '\n';
    }
}
void requireDacl(const DaclState& before, const fs::path& path, const char* message) {
    const auto after = fileDaclState(path);
    constexpr auto flags = SE_DACL_PRESENT | SE_DACL_DEFAULTED | SE_DACL_PROTECTED |
        SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED;
    const bool equal = before.sddl == after.sddl && (before.control & flags) == (after.control & flags);
    if (!equal) { describeDacl("before", before); describeDacl("after", after); }
    require(equal, message);
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

void protectiveScopeGuardsAndNewMatches() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"Guard scope 資料";
    const auto childPath = scopePath / L"target-folder", deepPath = childPath / L"deep";
    const auto peerPath = fixture.root / L"peer", outsidePath = fixture.root / L"Guard scope 資料 suffix";
    require(fs::create_directories(deepPath) && fs::create_directories(peerPath / L"deep") &&
            fs::create_directory(outsidePath) && fs::create_directory(scopePath / L"historic"),
            "create exclusively owned guard hierarchy");
    const std::vector<fs::path> initial{
        scopePath / L"target.txt", childPath, childPath / L"target.txt", deepPath / L"target.txt",
        peerPath / L"target.txt", peerPath / L"deep" / L"target.txt", outsidePath / L"target.txt",
        scopePath / L"historic" / L"target.txt"};
    for (size_t i = 0; i < initial.size(); ++i) if (i != 1) write(initial[i], "unchanged guard source");
    setOwnedTimestamp(initial.back(), localTimestamp(2000, 1, 1));
    std::vector<FileIdentity> initialIds;
    std::vector<FILE_BASIC_INFO> initialBasic;
    for (const auto& path : initial) { initialIds.push_back(fileIdentity(path.native())); initialBasic.push_back(fileBasic(path)); }
    auto scope = shellItem(scopePath), child = shellItem(childPath), peer = shellItem(peerPath);
    auto aliasPath = fixture.root / L"guard SCOPE 資料" / L".";
    const auto shortLength = GetShortPathNameW(scopePath.c_str(), nullptr, 0);
    bool distinctShortAlias = false;
    if (shortLength && shortLength <= 32768) {
        std::wstring shortPath(shortLength, L'\0');
        const auto copied = GetShortPathNameW(scopePath.c_str(), shortPath.data(), shortLength);
        if (copied && copied < shortLength) {
            shortPath.resize(copied);
            distinctShortAlias = _wcsicmp(shortPath.c_str(), scopePath.c_str()) != 0;
            if (distinctShortAlias) aliasPath = shortPath;
        }
    }
    auto alias = shellItem(aliasPath);
    require(fileIdentity(nativeFilesystemPath(alias.Get())) == fileIdentity(scopePath.native()),
            "guard alias changed actual native directory identity");
    std::cout << "INFO: protective scope owned alias shortDistinct=" << distinctShortAlias << '\n';

    // Native comparison uses every field rather than accepting a textual
    // prefix, a matching property alone, or a coincidentally equal file count.
    ComPtr<IConditionFactory2> factory;
    succeeded(CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
              "create real native guard comparator conditions");
    const auto canonicalPath = nativeFilesystemPath(scope.Get());
    const auto leaf = [&](const wchar_t* property, CONDITION_OPERATION operation, const std::wstring& text,
                          const wchar_t* semantic, VARTYPE type = VT_LPWSTR) {
        struct Value { PROPVARIANT value{}; ~Value() { PropVariantClear(&value); } } value;
        if (type == VT_BSTR) {
            value.value.vt = VT_BSTR; value.value.bstrVal = SysAllocString(text.c_str());
            require(value.value.bstrVal != nullptr, "allocate owned native guard BSTR");
        } else succeeded(InitPropVariantFromString(text.c_str(), &value.value), "allocate owned native guard literal");
        ComPtr<ICondition> condition;
        succeeded(factory->MakeLeaf(property, operation, semantic, &value.value, nullptr, nullptr, nullptr, FALSE, &condition),
                  "construct real native guard counterexample leaf");
        PROPVARIANT readback{}; CONDITION_OPERATION readOperation{}; PWSTR readProperty = nullptr;
        const auto readStatus = condition->GetComparisonInfo(&readProperty, &readOperation, &readback);
        const auto actualType = readback.vt;
        CoTaskMemFree(readProperty); PropVariantClear(&readback);
        succeeded(readStatus, "read native counterexample VARTYPE");
        require(actualType == type, "native counterexample did not retain its requested actual VARTYPE");
        return condition;
    };
    constexpr auto stringType = L"System.StructuredQueryType.String";
    auto expectedLeaf = leaf(L"System.ItemFolderPathDisplay", COP_EQUAL, canonicalPath, stringType);
    const auto compare = [&](ICondition* actual, bool expected) {
        bool same = !expected;
        succeeded(explorer::search_scope_internal::sameScopeGuard(expectedLeaf.Get(), actual, &same),
                  "compare complete native guard fields");
        require(same == expected, "native guard comparator lost a complete leaf/connective field");
    };
    compare(expectedLeaf.Get(), true);
    for (auto different : {
        leaf(L"System.ItemFolderPathDisplay", COP_EQUAL, canonicalPath + L" suffix", stringType),
        leaf(L"System.ItemFolderPathDisplay", COP_NOTEQUAL, canonicalPath, stringType),
        leaf(L"System.ItemPathDisplay", COP_EQUAL, canonicalPath, stringType),
        leaf(L"System.ItemFolderPathDisplay", COP_EQUAL, canonicalPath, nullptr),
        leaf(L"System.ItemFolderPathDisplay", COP_EQUAL, canonicalPath, stringType, VT_BSTR)}) compare(different.Get(), false);
    ComPtr<ICondition> inverse, repeated;
    succeeded(factory->MakeNot(expectedLeaf.Get(), FALSE, &inverse), "construct real native NOT counterexample");
    compare(inverse.Get(), false);
    ICondition* duplicates[]{expectedLeaf.Get(), expectedLeaf.Get()};
    succeeded(factory->CreateCompoundFromArray(CT_OR_CONDITION, duplicates, 2, CONDITION_CREATION_DEFAULT,
                                               IID_PPV_ARGS(&repeated)), "construct real native duplicate OR");
    compare(repeated.Get(), true);
    auto otherLeaf = leaf(L"System.ItemFolderPathDisplay", COP_EQUAL, nativeFilesystemPath(peer.Get()), stringType);
    ICondition* distinct[]{expectedLeaf.Get(), otherLeaf.Get()};
    ComPtr<ICondition> conjunction, disjunction;
    succeeded(factory->CreateCompoundFromArray(CT_AND_CONDITION, distinct, 2, CONDITION_CREATION_DEFAULT,
                                               IID_PPV_ARGS(&conjunction)), "construct real native distinct AND");
    succeeded(factory->CreateCompoundFromArray(CT_OR_CONDITION, distinct, 2, CONDITION_CREATION_DEFAULT,
                                               IID_PPV_ARGS(&disjunction)), "construct real native distinct OR");
    bool same = true;
    succeeded(explorer::search_scope_internal::sameScopeGuard(conjunction.Get(), disjunction.Get(), &same),
              "compare real native distinct Boolean connectives");
    require(!same, "native guard comparator conflated distinct AND and OR");
    same = true;
    require(explorer::search_scope_internal::sameScopeGuard(nullptr, expectedLeaf.Get(), &same) == E_INVALIDARG && same,
            "failed native guard comparison changed its public output");
    bool required = true;
    require(explorer::search_scope_internal::requiresProtectiveGuard({}, &required) == unsupported && required,
            "failed guard eligibility changed its public output");
    ICondition* retained = expectedLeaf.Get();
    retained->AddRef();
    const auto invalidGuard = explorer::search_scope_internal::createScopeGuard({{nullptr, true, false}}, &retained);
    const bool retainedOutput = retained == expectedLeaf.Get();
    if (retained) retained->Release();
    require(invalidGuard == E_INVALIDARG && retainedOutput, "failed guard construction replaced its public native output");

    using Rule = explorer::SearchScopeRule;
    struct Example {
        std::vector<Rule> rules;
        std::vector<size_t> members, added;
        fs::path saved, again;
        explorer::SavedSearchMetadata imported;
        std::string bytes;
        FileIdentity savedId;
    };
    std::vector<Example> examples{
        {{{scope, true, false}, {child, false, true}, {peer, true, false}}, {0, 3, 4, 5}, {0, 2, 3}},
        // A shallow exclusion also removes the immediate child folder object;
        // files within that folder and all deeper matches remain eligible.
        {{{scope, true, false}, {alias, false, true}, {peer, true, false}}, {2, 3, 4, 5}, {1, 2, 3}},
        {{{scope, false, false}, {child, true, true}, {peer, true, false}}, {0, 4, 5}, {0, 3}},
        {{{scope, true, false}, {alias, true, true}, {child, true, false}, {peer, true, false}}, {4, 5}, {3}},
        {{{scope, false, false}, {alias, true, true}}, {}, {}}
    };
    const std::wstring query = L"(System.FileName:=\"target.txt\" OR System.FileName:=\"target-new.txt\" OR "
        L"System.FileName:=\"target-folder\") AND NOT System.FileName:=\"unmatched.txt\" AND "
        L"System.DateModified:System.StructuredQueryType.DateTime#Today";
    const auto expectedPaths = [&](const Example& example, const std::vector<fs::path>& added) {
        std::set<std::wstring> result;
        for (auto index : example.members) result.insert(initial[index].native());
        for (auto index : example.added) if (!added.empty()) result.insert(added[index].native());
        return result;
    };
    const auto verify = [&](IShellItem* search, const std::set<std::wstring>& expected, size_t example, unsigned route) {
        const auto actual = searchResults(search);
        std::set<FileIdentity> actualIds, expectedIds;
        for (const auto& path : actual) actualIds.insert(fileIdentity(path));
        for (const auto& path : expected) expectedIds.insert(fileIdentity(path));
        const bool equal = actual.size() == expected.size() && actualIds == expectedIds && actual.size() == actualIds.size();
        if (!equal) {
            std::cerr << "protectiveScope example=" << example << " route=" << route << " expected=" << expected.size()
                      << " actual=" << actual.size() << " distinct=" << actualIds.size() << '\n';
            const auto identityEvidence = [](const char* difference, const FileIdentity& identity) {
                std::cerr << "protectiveScope " << difference << " volume=" << identity.volume << " id=";
                constexpr char hex[] = "0123456789abcdef";
                for (auto byte : identity.identifier) std::cerr << hex[byte >> 4] << hex[byte & 15];
                std::cerr << '\n';
            };
            for (const auto& identity : expectedIds) if (!actualIds.contains(identity)) identityEvidence("missing", identity);
            for (const auto& identity : actualIds) if (!expectedIds.contains(identity)) identityEvidence("unexpected", identity);
            for (size_t i = 0; i < initialIds.size(); ++i)
                if (expectedIds.contains(initialIds[i]) != actualIds.contains(initialIds[i]))
                    std::cerr << "protectiveScope differenceOwnedMember=" << i << " expected=" << expectedIds.contains(initialIds[i])
                              << " actual=" << actualIds.contains(initialIds[i]) << '\n';
        }
        require(equal, "protective scope changed complete actual native FileID membership");
    };
    // The original native query must independently match the folder that a
    // shallow equal-root exclusion later removes, and both deeper files.
    // This makes absence a scope semantic assertion rather than an unnoticed
    // filename/date predicate mismatch.
    ComPtr<IShellItem> unrestricted;
    succeeded(explorer::createSearchFolderForScopeRules(query, {{scope, true, false}, {peer, true, false}}, &unrestricted),
              "create genuine recursive-union scope baseline");
    verify(unrestricted.Get(), {initial[0].native(), initial[1].native(), initial[2].native(),
                               initial[3].native(), initial[4].native(), initial[5].native()}, examples.size(), 8);
    unrestricted.Reset();
    for (size_t i = 0; i < examples.size(); ++i) {
        auto& example = examples[i]; required = false;
        succeeded(explorer::search_scope_internal::requiresProtectiveGuard(example.rules, &required), "inspect genuine unsafe physical scope");
        require(required, "unsafe physical scope did not require exact guard");
        ComPtr<ICondition> guard;
        succeeded(explorer::search_scope_internal::createScopeGuard(example.rules, &guard), "create complete native protective predicate");
        ComPtr<IShellItem> live;
        succeeded(explorer::createSearchFolderForScopeRules(query, example.rules, &live), "create guarded native live scope");
        const auto expected = expectedPaths(example, {});
        verify(live.Get(), expected, i, 0);
        example.saved = fixture.root / (L"guard-" + std::to_wstring(i) + L".search-ms");
        example.again = fixture.root / (L"guard-again-" + std::to_wstring(i) + L".search-ms");
        succeeded(explorer::saveSearchForScopeRules(query, example.rules, example.saved), "save original query and exact scope guard");
        example.bytes = read(example.saved); example.savedId = fileIdentity(example.saved.native());
        verify(reopenSearch(example.saved).Get(), expected, i, 1);
        succeeded(explorer::readSavedSearch(example.saved, &example.imported), "import exact guard without editing source");
        require(example.imported.scopeRules.size() == example.rules.size(), "guard import changed scope count");
        const auto scopeFacts = [](const std::vector<Rule>& rules) {
            std::multiset<std::tuple<FileIdentity, bool, bool>> facts;
            for (const auto& rule : rules) {
                require(rule.folder != nullptr, "guard import lost actual folder identity");
                facts.emplace(fileIdentity(nativeFilesystemPath(rule.folder.Get())), rule.recursive, rule.excluded);
            }
            return facts;
        };
        // The public XML groups include elements before exclude elements;
        // preserve every rule and duplicate occurrence independently of order.
        require(scopeFacts(example.imported.scopeRules) == scopeFacts(example.rules),
                "guard import changed actual directory identity/recursion/exclusion or rule multiplicity");
        require(example.imported.query.find(L"System.ItemFolderPathDisplay") == std::wstring::npos &&
                example.imported.query.find(L"System.ItemPathDisplay") == std::wstring::npos,
                "scope guard leaked into editable original native query");
        live.Reset();
        succeeded(explorer::createSearchFolderForScopeRules(example.imported.query, example.imported.scopeRules, &live),
                  "restore imported original query and individual scope flags");
        verify(live.Get(), expected, i, 2);
        succeeded(explorer::saveSearchForScopeRules(example.imported.query, example.imported.scopeRules, example.again),
                  "re-save imported original query with recognized guard");
        verify(reopenSearch(example.again).Get(), expected, i, 3);
        require(example.bytes.find("R00UUUUUUUUZDNNU") != std::string::npos && read(example.again).find("R00UUUUUUUUZDNNU") != std::string::npos,
                "protective scope froze original unresolved Today condition");
    }

    const std::vector<fs::path> added{
        scopePath / L"target-new.txt", childPath / L"target-new.txt", deepPath / L"target-new.txt",
        peerPath / L"target-new.txt", outsidePath / L"target-new.txt"};
    for (const auto& path : added) write(path, "new match after original saved publication");
    std::vector<FileIdentity> addedIds;
    std::vector<FILE_BASIC_INFO> addedBasic;
    for (const auto& path : added) { addedIds.push_back(fileIdentity(path.native())); addedBasic.push_back(fileBasic(path)); }
    for (size_t i = 0; i < examples.size(); ++i) {
        const auto& example = examples[i]; const auto expected = expectedPaths(example, added);
        ComPtr<IShellItem> live;
        succeeded(explorer::createSearchFolderForScopeRules(query, example.rules, &live), "rerun guarded live query on new owned matches");
        verify(live.Get(), expected, i, 4);
        requireReopenedResults(example.saved, expected, "original protective saved query froze matching FileIDs");
        live.Reset();
        succeeded(explorer::createSearchFolderForScopeRules(example.imported.query, example.imported.scopeRules, &live),
                  "rerun guarded imported query on new owned matches");
        verify(live.Get(), expected, i, 6);
        requireReopenedResults(example.again, expected, "re-saved protective query froze matching FileIDs");
        require(read(example.saved) == example.bytes && fileIdentity(example.saved.native()) == example.savedId,
                "native protective query execution changed original saved bytes or FileID");
    }
    const auto& first = examples.front();
    const auto outputMembers = fixtureMembers(fixture.root);
    for (const auto& invalid : {std::vector<Rule>{{nullptr, true, false}}, std::vector<Rule>(257, {scope, true, false})}) {
        require(FAILED(explorer::saveSearchForScopeRules(query, invalid, first.saved, explorer::SearchSaveMode::UserConfirmed)),
                "invalid protective scope accepted confirmed overwrite");
        require(read(first.saved) == first.bytes && fileIdentity(first.saved.native()) == first.savedId &&
                fixtureMembers(fixture.root) == outputMembers, "invalid protective scope changed output or leaked staging file");
    }
    ComPtr<IShellItem> computer;
    succeeded(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&computer)),
              "get native This PC metadata only for unsupported mixed guard");
    const auto rejected = fixture.root / L"unsafe-virtual.search-ms";
    require(explorer::saveSearchForScopeRules(query, {{computer, true, false}, {child, false, true}}, rejected) == unsupported &&
            !fs::exists(rejected), "unsafe virtual protective scope was partially written");
    for (size_t i = 0; i < initial.size(); ++i) {
        require(fileIdentity(initial[i].native()) == initialIds[i], "protective query changed original member FileID");
        if (i != 1) require(read(initial[i]) == "unchanged guard source" &&
                            fileBasic(initial[i]).LastWriteTime.QuadPart == initialBasic[i].LastWriteTime.QuadPart,
                            "protective query changed original member bytes/timestamp");
    }
    for (size_t i = 0; i < added.size(); ++i)
        require(fileIdentity(added[i].native()) == addedIds[i] && read(added[i]) == "new match after original saved publication" &&
                fileBasic(added[i]).LastWriteTime.QuadPart == addedBasic[i].LastWriteTime.QuadPart,
                "protective query changed newly matching source bytes/FileID/timestamp");
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
    const auto beforeDacl = fileDaclState(output);
    const auto dacl = beforeDacl.sddl;
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
    requireDacl(beforeDacl, output, "confirmed replacement changed the existing saved-query DACL");
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
    const auto beforeRuleBytes = read(output);
    const auto beforeRuleId = fileIdentity(output.native());
    const auto beforeRuleBasic = fileBasic(output);
    succeeded(explorer::saveSearchForScopeRules(L"System.FileName:=\"second.bin\"", rules, output,
        explorer::SearchSaveMode::UserConfirmed), "replace confirmed explicit scope-rule query");
    requireReopenedResults(output, {second.native()}, "rule replacement changed native scope semantics",
        [&](const std::set<std::wstring>& actual) {
            const auto identity = [](unsigned stage, unsigned ordinal, const FileIdentity& id) {
                std::cerr << "INFO: confirmedRule identity stage=" << stage << " ordinal=" << ordinal
                          << " volume=" << id.volume << " id=";
                constexpr char hex[] = "0123456789abcdef";
                for (const auto byte : id.identifier) std::cerr << hex[byte >> 4] << hex[byte & 15];
                std::cerr << '\n';
            };
            const auto bytes = [&](unsigned stage, const std::string& value, const FileIdentity& id,
                                   const FILE_BASIC_INFO& basicInfo) {
                std::cerr << "INFO: confirmedRule publication stage=" << stage << " bytes=" << value.size()
                          << " firstToken=" << (value.find("first.txt") != std::string::npos)
                          << " secondToken=" << (value.find("second.bin") != std::string::npos)
                          << " attributes=" << basicInfo.FileAttributes
                          << " creation=" << basicInfo.CreationTime.QuadPart
                          << " write=" << basicInfo.LastWriteTime.QuadPart
                          << " change=" << basicInfo.ChangeTime.QuadPart << '\n';
                identity(stage, 0, id);
            };
            const auto scopeEvidence = [&](unsigned stage, const std::vector<explorer::SearchScopeRule>& values) {
                std::cerr << "INFO: confirmedRule scopes stage=" << stage << " count=" << values.size() << '\n';
                for (size_t index = 0; index < values.size(); ++index) {
                    const auto& rule = values[index];
                    std::cerr << "INFO: confirmedRule scope stage=" << stage << " ordinal=" << index
                              << " recursive=" << rule.recursive << " excluded=" << rule.excluded
                              << " present=" << (rule.folder != nullptr) << '\n';
                    if (rule.folder) identity(stage, static_cast<unsigned>(index),
                        fileIdentity(nativeFilesystemPath(rule.folder.Get())));
                }
            };
            const auto membersEvidence = [&](unsigned stage, const std::set<std::wstring>& values) {
                std::cerr << "INFO: confirmedRule members stage=" << stage << " count=" << values.size() << '\n';
                unsigned ordinal = 0;
                for (const auto& value : values) identity(stage, ordinal++, fileIdentity(value));
            };
            // Numeric stages: 1 prior union publication, 2 current rule
            // publication, 3 expected scope, 4 imported scope, 5 captured
            // failed native result, 6 expected result, 7 fresh rule live,
            // 8 imported live. Diagnostics never re-save or notify the Shell.
            bytes(1, beforeRuleBytes, beforeRuleId, beforeRuleBasic);
            const auto afterBytes = read(output);
            bytes(2, afterBytes, fileIdentity(output.native()), fileBasic(output));
            std::cerr << "INFO: confirmedRule publication changedBytes=" << (beforeRuleBytes != afterBytes) << '\n';
            scopeEvidence(3, rules);
            membersEvidence(5, actual);
            membersEvidence(6, {second.native()});
            explorer::SavedSearchMetadata metadata;
            const auto importedStatus = explorer::readSavedSearch(output, &metadata);
            std::cerr << "INFO: confirmedRule import HRESULT=0x" << std::hex
                      << static_cast<unsigned long>(importedStatus) << std::dec
                      << " queryUnits=" << metadata.query.size()
                      << " firstToken=" << (metadata.query.find(L"first.txt") != std::wstring::npos)
                      << " secondToken=" << (metadata.query.find(L"second.bin") != std::wstring::npos)
                      << " recursive=" << metadata.recursive << '\n';
            if (SUCCEEDED(importedStatus)) scopeEvidence(4, metadata.scopeRules);
            const auto liveEvidence = [&](unsigned stage, const std::wstring& query,
                                          const std::vector<explorer::SearchScopeRule>& values) {
                ComPtr<IShellItem> live;
                const auto status = explorer::createSearchFolderForScopeRules(query, values, &live);
                std::cerr << "INFO: confirmedRule live stage=" << stage << " HRESULT=0x" << std::hex
                          << static_cast<unsigned long>(status) << std::dec << '\n';
                if (SUCCEEDED(status)) membersEvidence(stage, searchResults(live.Get()));
            };
            liveEvidence(7, L"System.FileName:=\"second.bin\"", rules);
            if (SUCCEEDED(importedStatus)) liveEvidence(8, metadata.query, metadata.scopeRules);
        });
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

std::wstring currentFixtureTrustee() {
    OwnedHandle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE, "read current owned fixture trustee token");
    DWORD length = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
    require(length != 0, "read owned fixture trustee length");
    std::vector<BYTE> bytes(length);
    require(GetTokenInformation(token.value, TokenUser, bytes.data(), length, &length) != FALSE, "read owned fixture trustee");
    PWSTR raw = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &raw) != FALSE && raw,
            "encode owned fixture trustee internally");
    const std::wstring result(raw); LocalFree(raw); return result;
}
void setLegacyFixtureDacl(const fs::path& path, const std::wstring& sddl) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr) != FALSE,
            "create exclusively owned legacy ACL fixture descriptor");
    // The obsolete but documented low-level setter is used ONLY to generate
    // legacy control/ACE fixtures. Production never uses it to set a file.
    const auto success = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, raw);
    LocalFree(raw); require(success != FALSE, "set exclusively owned legacy ACL fixture");
}
void modernizeFixtureDacl(const fs::path& path, bool protectedDacl) {
    const auto state = fileDaclState(path); PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    require(GetSecurityDescriptorDacl(const_cast<BYTE*>(state.descriptor.data()), &present, &acl, &defaulted) != FALSE,
            "read owned fixture ACL for modern inheritance");
    OwnedHandle file{CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "open exclusively owned inheritance fixture");
    require(SetSecurityInfo(file.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
        (protectedDacl ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
        nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS, "convert only owned ACL to actual native auto-inheritance");
}
void confirmedSearchSecurityProfiles() {
    Fixture fixture;
    const auto scopePath = fixture.root / L"scope"; require(fs::create_directory(scopePath), "create owned security query scope");
    const auto first = scopePath / L"first.txt", second = scopePath / L"second.bin";
    write(first, "unchanged owned permission fixture A"); write(second, "unchanged owned permission fixture B");
    const auto firstId = fileIdentity(first.native()), secondId = fileIdentity(second.native());
    const auto firstBasic = fileBasic(first), secondBasic = fileBasic(second);
    auto scope = shellItem(scopePath); const auto trustee = currentFixtureTrustee();
    const auto parent = fixture.root / L"legacy-parent"; require(fs::create_directory(parent), "create owned legacy permission parent");
    setLegacyFixtureDacl(parent, L"D:P(A;OICI;FA;;;" + trustee + L")(A;OICI;FR;;;WD)(A;OICI;0x1200a9;;;BU)");
    for (const auto* profile : {L"legacy-inherited", L"legacy-explicit", L"protected", L"modern-inherited", L"modern-protected", L"deny-data-write"}) {
        const std::wstring name(profile); const auto output = parent / (name + L".search-ms");
        succeeded(explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output), "create owned exact-ACL query fixture");
        if (name == L"legacy-explicit") setLegacyFixtureDacl(output, L"D:(A;;FA;;;" + trustee + L")(A;;FR;;;WD)");
        if (name == L"protected" || name == L"modern-protected")
            setLegacyFixtureDacl(output, L"D:P(A;;FA;;;" + trustee + L")(A;;FR;;;WD)");
        if (name == L"deny-data-write")
            setLegacyFixtureDacl(output, L"D:P(D;;0x2;;;WD)(A;;FA;;;" + trustee + L")(A;;FR;;;WD)");
        if (name == L"modern-inherited" || name == L"modern-protected") modernizeFixtureDacl(output, name == L"modern-protected");
        const auto beforeDacl = fileDaclState(output); const auto beforeBasic = fileBasic(output);
        const auto beforeBytes = read(output); const auto beforeMembers = fixtureMembers(fixture.root);
        const bool modern = name.starts_with(L"modern");
        require(((beforeDacl.control & SE_DACL_AUTO_INHERITED) != 0) == modern,
                "Owned legacy/modern permission fixture did not establish its intended actual native state");
        const auto replaced = explorer::saveSearch(L"System.FileName:=\"second.bin\"", scope.Get(), true, output,
            explorer::SearchSaveMode::UserConfirmed);
        if (FAILED(replaced)) { describeDacl("owned profile original", beforeDacl); describeDacl("owned profile after failure", fileDaclState(output)); }
        succeeded(replaced, "replace actual native protected/legacy/modern/deny-permission saved query");
        require(read(output) != beforeBytes && read(output).find("second.bin") != std::string::npos,
                "Exact-ACL replacement did not publish complete updated XML");
        requireDacl(beforeDacl, output, "Actual confirmed query replacement changed exact ACE order/rights/flags or DACL control");
        const auto afterBasic = fileBasic(output);
        require(afterBasic.CreationTime.QuadPart == beforeBasic.CreationTime.QuadPart &&
                afterBasic.FileAttributes == beforeBasic.FileAttributes && fixtureMembers(fixture.root) == beforeMembers,
                "Exact-ACL replacement changed original metadata or left a staging entry");
        requireReopenedResults(output, {second.native()}, "Exact-ACL replacement has incorrect actual native result identity");
        const auto finalBytes = read(output); const auto finalId = fileIdentity(output.native());
        OwnedHandle locked{CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(locked.value != INVALID_HANDLE_VALUE, "lock only owned exact-ACL query against replacement");
        require(explorer::saveSearch(L"System.FileName:=\"first.txt\"", scope.Get(), true, output,
            explorer::SearchSaveMode::UserConfirmed) == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            "Exact-ACL sharing failure was ignored");
        require(read(output) == finalBytes && fileIdentity(output.native()) == finalId && fixtureMembers(fixture.root) == beforeMembers,
                "Failed exact-ACL replacement changed existing content/identity or left a staging entry");
        requireDacl(beforeDacl, output, "Failed actual exact-ACL replacement modified original permissions");
        CloseHandle(locked.value); locked.value = INVALID_HANDLE_VALUE;

        // The app-owned presentation record uses the same safe metadata
        // publication, in an explicitly owned cache rather than LocalAppData.
        const auto cache = parent / (name + L"-owned-cache");
        explorer::SearchViewPresentation presentation;
        presentation.mode = explorer::SearchViewMode::Content; presentation.iconSize = 32;
        succeeded(explorer::saveSearchPresentationCompanion(output, cache, presentation), "create only owned presentation ACL fixture");
        auto entries = fs::directory_iterator(cache);
        require(entries != fs::directory_iterator{}, "owned companion fixture is missing");
        const auto record = entries->path();
        ++entries; require(entries == fs::directory_iterator{}, "owned companion fixture unexpectedly has multiple records");
        if (name == L"legacy-explicit") setLegacyFixtureDacl(record, L"D:(A;;FA;;;" + trustee + L")(A;;FR;;;WD)");
        if (name == L"protected" || name == L"modern-protected")
            setLegacyFixtureDacl(record, L"D:P(A;;FA;;;" + trustee + L")(A;;FR;;;WD)");
        if (name == L"deny-data-write")
            setLegacyFixtureDacl(record, L"D:P(D;;0x2;;;WD)(A;;FA;;;" + trustee + L")(A;;FR;;;WD)");
        if (modern) modernizeFixtureDacl(record, name == L"modern-protected");
        const auto recordDacl = fileDaclState(record);
        const auto queryBeforeCompanion = read(output); const auto queryIdBeforeCompanion = fileIdentity(output.native());
        presentation.mode = explorer::SearchViewMode::List; presentation.iconSize = 16;
        succeeded(explorer::saveSearchPresentationCompanion(output, cache, presentation), "replace owned companion retaining exact ACL");
        requireDacl(recordDacl, record, "Actual companion replacement changed exact native ACL or inheritance control");
        explorer::SearchViewPresentation loaded;
        require(explorer::loadSearchPresentationCompanion(output, cache, &loaded) == S_OK && loaded.mode == presentation.mode &&
                loaded.iconSize == presentation.iconSize && read(output) == queryBeforeCompanion &&
                fileIdentity(output.native()) == queryIdBeforeCompanion,
                "Exact-ACL companion replacement lost layout or changed the saved query");
        require(std::distance(fs::directory_iterator(cache), fs::directory_iterator{}) == 1,
                "Exact-ACL companion replacement leaked a staging record");
    }
    require(fileIdentity(first.native()) == firstId && fileIdentity(second.native()) == secondId &&
            read(first) == "unchanged owned permission fixture A" && read(second) == "unchanged owned permission fixture B" &&
            fileBasic(first).LastWriteTime.QuadPart == firstBasic.LastWriteTime.QuadPart &&
            fileBasic(second).LastWriteTime.QuadPart == secondBasic.LastWriteTime.QuadPart,
            "Exact-permission saved-query fixtures modified source identities/content/times");
}
} // namespace

int runSearchSecurityTests() {
    try { confirmedSearchSecurityProfiles(); std::cout << "PASS: actual confirmed saves preserve six legacy/protected/modern/deny DACL profiles\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL: actual native saved-search DACL profiles: " << error.what() << '\n'; return 1; }
}

int runSearchTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"native live/saved recursive and shallow fixture results", shallowAndRecursiveResults},
        {"canonical kind/date/size refinements and relative dates", canonicalRefinementsAndRelativeDates},
        {"typed day/range inclusive boundaries and live/saved/restored native identities", absoluteDateDayAndRangeResults},
        {"native saved numeric/string/wildcard/Boolean comparison results", comparisonOperatorSemantics},
        {"saved-search XML escaping, Unicode and This PC identity", xmlEscapingAndThisPcScope},
        {"full-field native scope guards, aliases and newly matching four-route FileIDs", protectiveScopeGuardsAndNewMatches},
        {"confirmed saved-search replacement, native results, permissions and failure preservation", confirmedSearchReplacement},
        {"confirmed saves preserve exact legacy/protected/modern/deny ACLs and native identities", confirmedSearchSecurityProfiles},
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
