#include "explorer/saved_search.hpp"
#include "saved_search_internal.hpp"
#include "search_scope_internal.hpp"
#include <msxml6.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <structuredquery.h>
#include <shlobj.h>
#include <propvarutil.h>
#include <propsys.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cwctype>
#include <map>
#include <memory>
#include <new>
#include <string_view>
#include <vector>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
constexpr HRESULT invalidData = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
constexpr size_t maximumBytes = 1024 * 1024;
constexpr unsigned maximumNodes = 4096;
constexpr unsigned maximumDepth = 72;
constexpr size_t maximumQueryLength = 32768;

struct Bstr {
    BSTR value = nullptr;
    Bstr() = default;
    explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    std::wstring text() const { return value ? std::wstring(value, SysStringLen(value)) : std::wstring(); }
};
struct Variant {
    VARIANT value{};
    ~Variant() { VariantClear(&value); }
};
struct PropertyVariant {
    PROPVARIANT value{};
    ~PropertyVariant() { PropVariantClear(&value); }
};
struct TaskFree { void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); } };
using TaskString = std::unique_ptr<wchar_t, TaskFree>;
struct HandleClose { void operator()(void* value) const noexcept { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using FileHandle = std::unique_ptr<void, HandleClose>;
using Attributes = std::map<std::wstring, std::wstring>;
using Elements = std::vector<ComPtr<IXMLDOMNode>>;

HRESULT property(IXMLDOMDocument2* document, const wchar_t* name, VARIANT value) {
    Bstr key(name);
    return key.value ? document->setProperty(key.value, value) : E_OUTOFMEMORY;
}

HRESULT parseDocument(VARIANT source, IXMLDOMDocument2** result) {
    ComPtr<IXMLDOMDocument2> document;
    auto hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = document->put_async(VARIANT_FALSE)) ||
        FAILED(hr = document->put_preserveWhiteSpace(VARIANT_TRUE)) ||
        FAILED(hr = document->put_validateOnParse(VARIANT_FALSE)) ||
        FAILED(hr = document->put_resolveExternals(VARIANT_FALSE))) return hr;
    VARIANT option{}; option.vt = VT_BOOL; option.boolVal = VARIANT_TRUE;
    if (FAILED(hr = property(document.Get(), L"ProhibitDTD", option))) return hr;
    option.boolVal = VARIANT_FALSE;
    if (FAILED(hr = property(document.Get(), L"UseInlineSchema", option))) return hr;
    option.vt = VT_I4; option.lVal = maximumDepth;
    if (FAILED(hr = property(document.Get(), L"MaxElementDepth", option))) return hr;
    VARIANT_BOOL loaded = VARIANT_FALSE;
    hr = document->load(source, &loaded);
    if (FAILED(hr)) return hr;
    if (loaded != VARIANT_TRUE) return invalidData;
    return document.CopyTo(result);
}

HRESULT parseBytes(std::string_view bytes, IXMLDOMDocument2** result) {
    Variant source;
    source.value.vt = VT_ARRAY | VT_UI1;
    source.value.parray = SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(bytes.size()));
    if (!source.value.parray) return E_OUTOFMEMORY;
    void* buffer = nullptr;
    auto hr = SafeArrayAccessData(source.value.parray, &buffer);
    if (FAILED(hr)) return hr;
    std::copy(bytes.begin(), bytes.end(), static_cast<char*>(buffer));
    if (FAILED(hr = SafeArrayUnaccessData(source.value.parray))) return hr;
    return parseDocument(source.value, result);
}

HRESULT loadDocument(const std::filesystem::path& path, IXMLDOMDocument2** result, std::string& bytes) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) return HRESULT_FROM_WIN32(GetLastError());
    if (size.QuadPart <= 0) return invalidData;
    if (size.QuadPart > static_cast<LONGLONG>(maximumBytes)) return unsupported;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD received = 0;
    const bool read = ReadFile(file.get(), bytes.data(), static_cast<DWORD>(size.QuadPart), &received, nullptr) != FALSE;
    const auto error = GetLastError();
    if (!read) return HRESULT_FROM_WIN32(error);
    if (received != static_cast<DWORD>(size.QuadPart)) return invalidData;
    return parseBytes(bytes, result);
}

HRESULT nodeName(IXMLDOMNode* node, std::wstring& result) {
    Bstr name;
    auto hr = node->get_nodeName(&name.value);
    if (FAILED(hr)) return hr;
    result = name.text();
    return S_OK;
}

bool whitespace(std::wstring_view text) {
    return text.find_first_not_of(L" \t\r\n") == std::wstring_view::npos;
}

HRESULT boundedTree(IXMLDOMNode* node, unsigned depth, unsigned& count) {
    if (depth > maximumDepth || ++count > maximumNodes) return unsupported;
    DOMNodeType type{};
    auto hr = node->get_nodeType(&type);
    if (FAILED(hr)) return hr;
    if (type == NODE_DOCUMENT_TYPE || type == NODE_ENTITY || type == NODE_ENTITY_REFERENCE) return unsupported;
    ComPtr<IXMLDOMNodeList> children;
    hr = node->get_childNodes(&children);
    if (FAILED(hr)) return hr;
    long length = 0;
    if (FAILED(hr = children->get_length(&length))) return hr;
    if (length > static_cast<long>(maximumNodes)) return unsupported;
    for (long i = 0; i < length; ++i) {
        ComPtr<IXMLDOMNode> child;
        if (FAILED(hr = children->get_item(i, &child))) return hr;
        if (!child) return invalidData;
        if (FAILED(hr = boundedTree(child.Get(), depth + 1, count))) return hr;
    }
    return S_OK;
}

HRESULT attributes(IXMLDOMNode* node, std::initializer_list<const wchar_t*> allowed, Attributes& result) {
    ComPtr<IXMLDOMNamedNodeMap> attributes;
    auto hr = node->get_attributes(&attributes);
    if (FAILED(hr)) return hr;
    long length = 0;
    if (!attributes) return S_OK;
    if (FAILED(hr = attributes->get_length(&length))) return hr;
    if (length > static_cast<long>(allowed.size())) return unsupported;
    for (long i = 0; i < length; ++i) {
        ComPtr<IXMLDOMNode> attribute;
        if (FAILED(hr = attributes->get_item(i, &attribute))) return hr;
        std::wstring name;
        if (FAILED(hr = nodeName(attribute.Get(), name))) return hr;
        if (std::find_if(allowed.begin(), allowed.end(), [&](const wchar_t* key) { return name == key; }) == allowed.end())
            return unsupported;
        Bstr value;
        if (FAILED(hr = attribute->get_text(&value.value))) return hr;
        if (!result.emplace(name, value.text()).second) return invalidData;
    }
    return S_OK;
}

HRESULT elements(IXMLDOMNode* node, Elements& result) {
    ComPtr<IXMLDOMNodeList> children;
    auto hr = node->get_childNodes(&children);
    if (FAILED(hr)) return hr;
    long length = 0;
    if (FAILED(hr = children->get_length(&length))) return hr;
    for (long i = 0; i < length; ++i) {
        ComPtr<IXMLDOMNode> child;
        if (FAILED(hr = children->get_item(i, &child))) return hr;
        DOMNodeType type{};
        if (FAILED(hr = child->get_nodeType(&type))) return hr;
        if (type == NODE_ELEMENT) { result.push_back(child); continue; }
        if (type == NODE_COMMENT) continue;
        if (type != NODE_TEXT && type != NODE_CDATA_SECTION) return unsupported;
        Bstr text;
        if (FAILED(hr = child->get_text(&text.value))) return hr;
        if (!whitespace(text.text())) return unsupported;
    }
    return S_OK;
}

HRESULT emptyElement(IXMLDOMNode* node) {
    Elements children;
    const auto hr = elements(node, children);
    return FAILED(hr) ? hr : children.empty() ? S_OK : unsupported;
}

HRESULT filePropertyMetadata(IXMLDOMNode* node, SearchFileProperties& result) {
    Attributes values;
    auto hr = attributes(node, {}, values);
    if (FAILED(hr)) return hr;
    Elements fields;
    if (FAILED(hr = elements(node, fields))) return hr;
    if (fields.size() > 4) return unsupported;
    for (const auto& field : fields) {
        std::wstring name;
        if (FAILED(hr = nodeName(field.Get(), name))) return hr;
        auto* target = name == L"author" ? &result.author : name == L"kind" ? &result.kind :
            name == L"description" ? &result.description : name == L"tags" ? &result.tags : nullptr;
        if (!target || *target) return unsupported;
        values.clear();
        if (FAILED(hr = attributes(field.Get(), {}, values))) return hr;
        ComPtr<IXMLDOMNodeList> children;
        if (FAILED(hr = field->get_childNodes(&children))) return hr;
        long length = 0;
        if (FAILED(hr = children->get_length(&length))) return hr;
        std::wstring text;
        for (long index = 0; index < length; ++index) {
            ComPtr<IXMLDOMNode> child;
            if (FAILED(hr = children->get_item(index, &child))) return hr;
            DOMNodeType type{};
            if (FAILED(hr = child->get_nodeType(&type))) return hr;
            if (type == NODE_COMMENT) continue;
            if (type != NODE_TEXT && type != NODE_CDATA_SECTION) return unsupported;
            // Bound simple text/CDATA shapes without MSXML's text-accessor
            // trimming. Exact CR/LF character-reference values are recovered
            // from the originally read bytes by XmlLite below.
            Variant value;
            if (FAILED(hr = child->get_nodeValue(&value.value))) return hr;
            if (value.value.vt != VT_BSTR) return invalidData;
            if (value.value.bstrVal) text.append(value.value.bstrVal, SysStringLen(value.value.bstrVal));
            if (text.size() > maximumQueryLength) return unsupported;
        }
        *target = std::move(text);
    }
    return S_OK;
}

HRESULT exactFilePropertyText(std::string_view bytes, SearchFileProperties& result) {
    // MSXML normalizes even adjacent CR/LF character references. XmlLite reads
    // their literal values from the same bounded bytes whose complete shape
    // was already checked above; there is no second file read or path race.
    ComPtr<IXmlReader> reader;
    auto hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(hr = reader->SetProperty(XmlReaderProperty_XmlResolver, 0)) ||
        FAILED(hr = reader->SetProperty(XmlReaderProperty_MaxElementDepth, maximumDepth))) return hr;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    if (!stream) return E_OUTOFMEMORY;
    if (FAILED(hr = reader->SetInput(stream.Get()))) return hr;
    bool inProperties = false;
    std::optional<std::wstring>* field = nullptr;
    unsigned count = 0;
    XmlNodeType type{};
    while ((hr = reader->Read(&type)) == S_OK) {
        if (++count > maximumNodes * 2) return unsupported;
        UINT depth = 0;
        if (FAILED(hr = reader->GetDepth(&depth))) return hr;
        if (type == XmlNodeType_Element) {
            const wchar_t* name = nullptr; UINT length = 0;
            if (FAILED(hr = reader->GetQualifiedName(&name, &length))) return hr;
            const std::wstring_view tag(name, length);
            if (depth == 1 && tag == L"properties") inProperties = !reader->IsEmptyElement();
            else if (inProperties && depth == 2) {
                field = tag == L"author" ? &result.author : tag == L"kind" ? &result.kind :
                    tag == L"description" ? &result.description : tag == L"tags" ? &result.tags : nullptr;
                if (!field || !*field) return unsupported;
                field->emplace();
                if (reader->IsEmptyElement()) field = nullptr;
            }
        } else if (type == XmlNodeType_EndElement) {
            // XmlLite reports an end tag before decrementing its depth. All
            // field shapes were validated as simple text, so their matching
            // end tag closes the active field regardless of that depth value.
            if (field) field = nullptr;
            const wchar_t* name = nullptr; UINT length = 0;
            if (FAILED(hr = reader->GetQualifiedName(&name, &length))) return hr;
            if (std::wstring_view(name, length) == L"properties") inProperties = false;
        } else if (field && (type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)) {
            const wchar_t* value = nullptr; UINT length = 0;
            if (FAILED(hr = reader->GetValue(&value, &length))) return hr;
            if (length > maximumQueryLength - field->value().size()) return unsupported;
            if (length) field->value().append(value, length);
        }
    }
    return hr == S_FALSE ? S_OK : hr;
}

HRESULT viewMetadata(IXMLDOMNode* node, SearchViewPresentation& result) {
    SearchViewPresentation candidate;
    Attributes values;
    auto hr = attributes(node, {L"viewMode", L"iconSize"}, values);
    if (FAILED(hr)) return hr;
    if (const auto mode = values.find(L"viewMode"); mode != values.end()) {
        auto text = mode->second;
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (text == L"details") candidate.mode = SearchViewMode::Details;
        else if (text == L"icons") candidate.mode = SearchViewMode::Icons;
        else if (text == L"tiles") candidate.mode = SearchViewMode::Tiles;
        else return unsupported;
    }
    if (const auto size = values.find(L"iconSize"); size != values.end()) {
        if (size->second.empty() || size->second.size() > 3) return unsupported;
        int number = 0;
        for (const auto c : size->second) {
            if (c < L'0' || c > L'9') return unsupported;
            number = number * 10 + c - L'0';
        }
        if (number < 16 || number > 256) return unsupported;
        candidate.iconSize = number;
    }
    const auto orderedProperty = [&](IXMLDOMNode* item, SearchViewOrder& order) -> HRESULT {
        Attributes itemValues;
        auto status = attributes(item, {L"viewField", L"direction"}, itemValues);
        if (FAILED(status)) return status;
        if (itemValues.size() != 2 || FAILED(status = emptyElement(item))) return itemValues.size() == 2 ? status : unsupported;
        order.property = itemValues[L"viewField"];
        const auto& direction = itemValues[L"direction"];
        if (direction == L"ascending") order.direction = SORT_ASCENDING;
        else if (direction == L"descending") order.direction = SORT_DESCENDING;
        else return unsupported;
        return S_OK;
    };
    Elements children;
    if (FAILED(hr = elements(node, children))) return hr;
    for (const auto& child : children) {
        std::wstring name;
        if (FAILED(hr = nodeName(child.Get(), name))) return hr;
        if (name == L"groupBy" && !candidate.groupBy) {
            candidate.groupBy.emplace();
            if (FAILED(hr = orderedProperty(child.Get(), *candidate.groupBy))) return hr;
            continue;
        }
        const bool columns = name == L"visibleColumns";
        if ((!columns && name != L"sortList") || (columns ? candidate.visibleColumns.has_value() : candidate.sort.has_value())) return unsupported;
        Attributes containerValues;
        if (FAILED(hr = attributes(child.Get(), {}, containerValues))) return hr;
        Elements entries;
        if (FAILED(hr = elements(child.Get(), entries))) return hr;
        if (entries.size() > (columns ? 128u : 4u)) return unsupported;
        if (columns) candidate.visibleColumns.emplace(); else candidate.sort.emplace();
        for (const auto& entry : entries) {
            if (FAILED(hr = nodeName(entry.Get(), name))) return hr;
            if (name != (columns ? L"column" : L"sort")) return unsupported;
            if (columns) {
                Attributes columnValues;
                if (FAILED(hr = attributes(entry.Get(), {L"viewField"}, columnValues))) return hr;
                if (columnValues.size() != 1 || FAILED(hr = emptyElement(entry.Get()))) return columnValues.size() == 1 ? hr : unsupported;
                candidate.visibleColumns->push_back(columnValues[L"viewField"]);
            } else {
                SearchViewOrder order;
                if (FAILED(hr = orderedProperty(entry.Get(), order))) return hr;
                candidate.sort->push_back(std::move(order));
            }
        }
    }
    if (FAILED(hr = validateSearchViewPresentation(candidate))) return hr == E_INVALIDARG ? unsupported : hr;
    result = std::move(candidate);
    return S_OK;
}

HRESULT scopeItemMetadata(IXMLDOMNode* node, SavedSearchMetadata& result) {
    std::wstring name;
    auto hr = nodeName(node, name);
    if (FAILED(hr)) return hr;
    if (name != L"include" && name != L"exclude") return unsupported;
    const bool excluded = name == L"exclude";
    Attributes values;
    if (FAILED(hr = attributes(node, {L"path", L"knownFolder", L"nonRecursive"}, values))) return hr;
    if (FAILED(hr = emptyElement(node))) return hr;
    const auto path = values.find(L"path"), known = values.find(L"knownFolder");
    if ((path == values.end()) == (known == values.end())) return unsupported;
    const auto recursion = values.find(L"nonRecursive");
    if (recursion != values.end()) {
        if (recursion->second == L"true") result.recursive = false;
        else if (recursion->second != L"false") return unsupported;
    }
    // Provider-dependent physical exclusions are parsed losslessly here.
    // Their exact protective condition guard is required before the reader
    // publishes editable metadata; an unguarded external file stays native-only.
    if (path != values.end()) {
        // Environment expansion is part of the documented saved-scope format.
        if (path->second.empty() || path->second.find(L'\0') != std::wstring::npos) return invalidData;
        const auto length = ExpandEnvironmentStringsW(path->second.c_str(), nullptr, 0);
        if (!length || length > 32768) return unsupported;
        std::wstring expanded(length, L'\0');
        if (ExpandEnvironmentStringsW(path->second.c_str(), expanded.data(), length) != length) return invalidData;
        expanded.resize(length - 1);
        if (!std::filesystem::path(expanded).is_absolute()) return unsupported;
        if (FAILED(hr = SHCreateItemFromParsingName(expanded.c_str(), nullptr, IID_PPV_ARGS(&result.scope)))) return hr;
        TaskString canonical;
        PWSTR raw = nullptr;
        hr = result.scope->GetDisplayName(SIGDN_FILESYSPATH, &raw); canonical.reset(raw);
        if (FAILED(hr) || !raw || !*raw) return unsupported;
    } else {
        // Only accept canonical GUID syntax. Never pass a ProgID to a native
        // converter that could register it while reading an external file.
        const auto& text = known->second;
        if (text.size() != 38 || text.front() != L'{' || text.back() != L'}') return unsupported;
        for (size_t i = 1; i < 37; ++i) {
            const bool separator = i == 9 || i == 14 || i == 19 || i == 24;
            if (separator ? text[i] != L'-' : !iswxdigit(text[i])) return unsupported;
        }
        GUID identifier{};
        if (FAILED(hr = IIDFromString(text.c_str(), &identifier))) return hr;
        if (FAILED(hr = SHGetKnownFolderItem(identifier, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&result.scope)))) return hr;
        if (!result.recursive || excluded) {
            PWSTR raw = nullptr;
            hr = result.scope->GetDisplayName(SIGDN_FILESYSPATH, &raw);
            TaskString nativePath(raw);
            if (FAILED(hr) || !raw || !*raw) return unsupported;
        }
    }
    SFGAOF flags = 0;
    if (FAILED(hr = result.scope->GetAttributes(SFGAO_FOLDER, &flags))) return hr;
    if (!(flags & SFGAO_FOLDER)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    result.scopeRules.push_back({result.scope, result.recursive, excluded});
    return S_OK;
}

HRESULT scopeMetadata(IXMLDOMNode* node, SavedSearchMetadata& result, bool& needsGuard) {
    Attributes values;
    auto hr = attributes(node, {}, values);
    if (FAILED(hr)) return hr;
    Elements children;
    if (FAILED(hr = elements(node, children))) return hr;
    if (children.empty() || children.size() > 256) return unsupported;
    struct PidlFree {
        using pointer = PIDLIST_ABSOLUTE;
        void operator()(pointer value) const noexcept { CoTaskMemFree(value); }
    };
    std::vector<std::unique_ptr<ITEMIDLIST, PidlFree>> owned;
    std::vector<PCIDLIST_ABSOLUTE> pidls;
    for (const auto& child : children) {
        SavedSearchMetadata location;
        if (FAILED(hr = scopeItemMetadata(child.Get(), location))) return hr;
        const auto& rule = location.scopeRules.front();
        result.scopeRules.push_back(rule);
        if (rule.excluded) continue;
        if (!result.scope) { result.scope = location.scope; result.recursive = location.recursive; }
        PIDLIST_ABSOLUTE raw = nullptr;
        hr = SHGetIDListFromObject(location.scope.Get(), &raw);
        std::unique_ptr<ITEMIDLIST, PidlFree> pidl(raw);
        if (FAILED(hr)) return hr;
        if (!raw) return E_UNEXPECTED;
        pidls.push_back(raw); owned.push_back(std::move(pidl));
    }
    if (!result.scope || pidls.empty()) return unsupported;
    const bool shallow = std::any_of(result.scopeRules.begin(), result.scopeRules.end(), [](const SearchScopeRule& rule) {
        return !rule.excluded && !rule.recursive;
    });
    for (const auto& rule : result.scopeRules) {
        if (!rule.excluded && shallow) {
            PWSTR raw = nullptr;
            hr = rule.folder->GetDisplayName(SIGDN_FILESYSPATH, &raw); TaskString path(raw);
            if (FAILED(hr) || !raw || !*raw) return unsupported;
        }
    }
    bool required = false;
    if (FAILED(hr = search_scope_internal::requiresProtectiveGuard(result.scopeRules, &required))) return hr;
    if (FAILED(hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), pidls.data(), &result.scopes))) return hr;
    needsGuard = required;
    return S_OK;
}

bool operation(std::wstring_view name, CONDITION_OPERATION& result) {
    struct Mapping { std::wstring_view name; CONDITION_OPERATION operation; };
    constexpr std::array mappings{
        Mapping{L"imp", COP_IMPLICIT}, Mapping{L"eq", COP_EQUAL}, Mapping{L"neq", COP_NOTEQUAL},
        Mapping{L"lt", COP_LESSTHAN}, Mapping{L"gt", COP_GREATERTHAN}, Mapping{L"lte", COP_LESSTHANOREQUAL},
        Mapping{L"gte", COP_GREATERTHANOREQUAL}, Mapping{L"starts with", COP_VALUE_STARTSWITH},
        Mapping{L"ends with", COP_VALUE_ENDSWITH}, Mapping{L"contains", COP_VALUE_CONTAINS},
        Mapping{L"does not contain", COP_VALUE_NOTCONTAINS}, Mapping{L"matches", COP_DOSWILDCARDS},
        Mapping{L"word eq", COP_WORD_EQUAL}, Mapping{L"wordmatch", COP_WORD_STARTSWITH}};
    for (const auto& mapping : mappings) if (name == mapping.name) { result = mapping.operation; return true; }
    return false;
}

HRESULT sameBooleanLeaf(ICondition* expected, ICondition* actual, bool& same) {
    same = false;
    CONDITION_TYPE firstType{}, secondType{};
    auto hr = expected->GetConditionType(&firstType);
    if (SUCCEEDED(hr)) hr = actual->GetConditionType(&secondType);
    if (FAILED(hr)) return hr;
    if (firstType != CT_LEAF_CONDITION || secondType != CT_LEAF_CONDITION) return S_OK;
    PWSTR firstProperty = nullptr, secondProperty = nullptr, firstSemantic = nullptr, secondSemantic = nullptr;
    CONDITION_OPERATION firstOperation{}, secondOperation{};
    PropertyVariant firstValue, secondValue;
    hr = expected->GetComparisonInfo(&firstProperty, &firstOperation, &firstValue.value);
    TaskString firstName(firstProperty);
    if (FAILED(hr)) return hr;
    hr = actual->GetComparisonInfo(&secondProperty, &secondOperation, &secondValue.value);
    TaskString secondName(secondProperty);
    if (FAILED(hr)) return hr;
    hr = expected->GetValueType(&firstSemantic);
    TaskString firstMeaning(firstSemantic);
    if (FAILED(hr)) return hr;
    hr = actual->GetValueType(&secondSemantic);
    TaskString secondMeaning(secondSemantic);
    if (FAILED(hr)) return hr;
    same = firstProperty && secondProperty && _wcsicmp(firstProperty, secondProperty) == 0 &&
        firstOperation == secondOperation && firstValue.value.vt == VT_BOOL && secondValue.value.vt == VT_BOOL &&
        firstValue.value.boolVal == secondValue.value.boolVal &&
        ((firstSemantic == nullptr && secondSemantic == nullptr) ||
         (firstSemantic && secondSemantic && wcscmp(firstSemantic, secondSemantic) == 0));
    return S_OK;
}

HRESULT inferredBooleanCondition(IConditionFactory2* factory, IQueryParser* queryParser,
                                 const std::wstring& property, CONDITION_OPERATION comparison,
                                 const std::wstring& text, ICondition** result) {
    // The public format includes Boolean leaves with no propertyType. Infer
    // only the installed schema's scalar Boolean, never another scalar type
    // or an arbitrary string conversion.
    if (comparison != COP_EQUAL && comparison != COP_NOTEQUAL) return unsupported;
    const bool isTrue = CompareStringOrdinal(text.c_str(), static_cast<int>(text.size()), L"TRUE", 4, TRUE) == CSTR_EQUAL;
    const bool isFalse = CompareStringOrdinal(text.c_str(), static_cast<int>(text.size()), L"FALSE", 5, TRUE) == CSTR_EQUAL;
    if (!isTrue && !isFalse) return unsupported;
    ComPtr<IPropertyDescription> description;
    auto hr = PSGetPropertyDescriptionByName(property.c_str(), IID_PPV_ARGS(&description));
    if (FAILED(hr)) return hr;
    VARTYPE type = VT_EMPTY;
    if (FAILED(hr = description->GetPropertyType(&type))) return hr;
    if (type != VT_BOOL) return unsupported;
    PROPERTYKEY key{};
    if (FAILED(hr = description->GetPropertyKey(&key))) return hr;
    ComPtr<ICondition> candidate;
    hr = factory->CreateBooleanLeaf(key, comparison, isTrue ? TRUE : FALSE,
                                    CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&candidate));
    if (FAILED(hr)) return hr;
    PWSTR raw = nullptr;
    hr = queryParser->RestateToString(candidate.Get(), FALSE, &raw);
    TaskString restated(raw);
    if (FAILED(hr)) return hr;
    if (!raw || !*raw || wcslen(raw) > maximumQueryLength) return unsupported;
    ComPtr<IQuerySolution> solution;
    if (FAILED(hr = queryParser->Parse(raw, nullptr, &solution))) return hr;
    ComPtr<ICondition> reparsed, expected, actual;
    if (FAILED(hr = solution->GetQuery(&reparsed, nullptr))) return hr;
    SYSTEMTIME now{}; GetLocalTime(&now);
    if (FAILED(hr = factory->Resolve(candidate.Get(), SQRO_DONT_SPLIT_WORDS, &now, &expected)) ||
        FAILED(hr = solution->Resolve(reparsed.Get(), SQRO_DONT_SPLIT_WORDS, &now, &actual))) return hr;
    if (!expected || !actual) return E_UNEXPECTED;
    PWSTR rawExpectedMeaning = nullptr;
    hr = expected->GetValueType(&rawExpectedMeaning);
    TaskString expectedMeaning(rawExpectedMeaning);
    if (FAILED(hr)) return hr;
    if (!rawExpectedMeaning) {
        // CreateBooleanLeaf supplies the schema-keyed VT_BOOL but leaves its
        // semantic name absent. The native parser adds its registered Boolean
        // semantic on restatement. Attach that observed name through the public
        // factory, retaining every typed factory field, then require the same
        // complete leaf comparison below. This never discards a differing type.
        PWSTR rawMeaning = nullptr, rawProperty = nullptr;
        hr = actual->GetValueType(&rawMeaning);
        TaskString meaning(rawMeaning);
        if (FAILED(hr)) return hr;
        if (!rawMeaning || wcscmp(rawMeaning, L"System.StructuredQueryType.Boolean") != 0) return unsupported;
        CONDITION_OPERATION operation{};
        PropertyVariant scalar;
        hr = expected->GetComparisonInfo(&rawProperty, &operation, &scalar.value);
        TaskString canonicalProperty(rawProperty);
        if (FAILED(hr)) return hr;
        if (!rawProperty || scalar.value.vt != VT_BOOL) return unsupported;
        ComPtr<ICondition> enriched;
        hr = factory->MakeLeaf(rawProperty, operation, rawMeaning, &scalar.value,
                               nullptr, nullptr, nullptr, FALSE, &enriched);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = factory->Resolve(enriched.Get(), SQRO_DONT_SPLIT_WORDS, &now, &expected))) return hr;
        if (!expected) return E_UNEXPECTED;
        candidate = std::move(enriched);
    }
    bool same = false;
    if (FAILED(hr = sameBooleanLeaf(expected.Get(), actual.Get(), same))) return hr;
    return same ? candidate.CopyTo(result) : unsupported;
}

HRESULT condition(IXMLDOMNode* node, IConditionFactory2* factory, IQueryParser* queryParser,
                  ICondition** result, unsigned depth = 0) {
    if (depth > 64) return unsupported;
    std::wstring name;
    auto hr = nodeName(node, name);
    if (FAILED(hr)) return hr;
    if (name != L"condition") return unsupported;
    Attributes values;
    if (FAILED(hr = attributes(node, {L"type", L"property", L"propertyType", L"operator", L"value", L"valuetype", L"valueType"}, values))) return hr;
    const auto type = values.find(L"type");
    if (type == values.end()) return invalidData;
    Elements children;
    if (FAILED(hr = elements(node, children))) return hr;
    if (type->second == L"leafCondition") {
        if (!children.empty()) return unsupported;
        const auto propertyType = values.find(L"propertyType"), op = values.find(L"operator"), value = values.find(L"value");
        if (op == values.end() || value == values.end()) return invalidData;
        if (propertyType != values.end() && propertyType->second != L"wstr" && propertyType->second != L"string") return unsupported;
        CONDITION_OPERATION comparison{};
        if (!operation(op->second, comparison)) return unsupported;
        const auto propertyName = values.find(L"property");
        if (propertyName == values.end() || propertyName->second.empty() || propertyName->second.size() > 1024) return unsupported;
        auto semantic = values.find(L"valuetype");
        const auto otherCase = values.find(L"valueType");
        if (semantic != values.end() && otherCase != values.end()) return unsupported;
        if (semantic == values.end()) semantic = otherCase;
        if (semantic != values.end() && (semantic->second.empty() || semantic->second.size() > 1024)) return unsupported;
        if (semantic != values.end() && !saved_search_internal::supportedValueType(semantic->second)) return unsupported;
        if (value->second.size() > maximumQueryLength) return unsupported;
        if (propertyType == values.end()) {
            if (semantic != values.end() && semantic->second != L"System.StructuredQueryType.Boolean") return unsupported;
            return inferredBooleanCondition(factory, queryParser, propertyName->second, comparison, value->second, result);
        }
        PropertyVariant scalar;
        if (FAILED(hr = InitPropVariantFromString(value->second.c_str(), &scalar.value))) return hr;
        return factory->MakeLeaf(propertyName == values.end() ? nullptr : propertyName->second.c_str(), comparison,
            semantic == values.end() ? nullptr : semantic->second.c_str(), &scalar.value,
            nullptr, nullptr, nullptr, FALSE, result);
    }
    if (values.size() != 1) return unsupported;
    if (type->second == L"notCondition") {
        if (children.size() != 1) return unsupported;
        ComPtr<ICondition> child;
        if (FAILED(hr = condition(children[0].Get(), factory, queryParser, &child, depth + 1))) return hr;
        return factory->MakeNot(child.Get(), FALSE, result);
    }
    CONDITION_TYPE connective{};
    if (type->second == L"andCondition") connective = CT_AND_CONDITION;
    else if (type->second == L"orCondition") connective = CT_OR_CONDITION;
    else return unsupported;
    if (children.size() < 2) return unsupported;
    std::vector<ComPtr<ICondition>> owned;
    std::vector<ICondition*> raw;
    for (const auto& child : children) {
        ComPtr<ICondition> item;
        if (FAILED(hr = condition(child.Get(), factory, queryParser, &item, depth + 1))) return hr;
        raw.push_back(item.Get()); owned.push_back(item);
    }
    return factory->CreateCompoundFromArray(connective, raw.data(), static_cast<ULONG>(raw.size()),
                                           CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}

HRESULT removeProtectiveScopeGuard(ICondition* original, const std::vector<SearchScopeRule>& rules,
                                  ICondition** result) {
    CONDITION_TYPE type{};
    auto hr = original->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type != CT_AND_CONDITION) return unsupported;
    ComPtr<IEnumUnknown> enumeration;
    if (FAILED(hr = original->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
    std::vector<ComPtr<ICondition>> children;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) return hr;
        if (!unknown || children.size() == 2) return unsupported;
        ComPtr<ICondition> child;
        if (FAILED(hr = unknown.As(&child))) return hr;
        children.push_back(std::move(child));
    }
    if (children.size() != 2) return unsupported;
    ComPtr<ICondition> expected;
    if (FAILED(hr = search_scope_internal::createScopeGuard(rules, &expected))) return hr;
    if (!expected) return E_UNEXPECTED;
    std::array<bool, 2> matches{};
    for (size_t i = 0; i < children.size(); ++i)
        if (FAILED(hr = search_scope_internal::sameScopeGuard(expected.Get(), children[i].Get(), &matches[i]))) return hr;
    // One exact top-level guard can be removed because the returned scope rules
    // reapply that same condition in live/save routes. Do not guess a guard in
    // a user OR/NOT, accept a partial domain, or strip two ambiguous matches.
    if (matches[0] == matches[1]) return unsupported;
    return children[matches[0] ? 1 : 0].CopyTo(result);
}

HRESULT parser(IQueryParser** result) {
    ComPtr<IQueryParserManager> manager;
    auto hr = CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;
    ComPtr<IQueryParser> queryParser;
    if (FAILED(hr = manager->CreateLoadedParser(L"SystemIndex", GetUserDefaultUILanguage(), IID_PPV_ARGS(&queryParser)))) return hr;
    if (FAILED(hr = manager->InitializeOptions(FALSE, TRUE, queryParser.Get()))) return hr;
    struct DefaultProperty { const wchar_t* type; const wchar_t* name; };
    constexpr std::array defaults{
        DefaultProperty{L"System.StructuredQueryType.String", L"System.Generic.String"},
        DefaultProperty{L"System.StructuredQueryType.Integer", L"System.Generic.Integer"},
        DefaultProperty{L"System.StructuredQueryType.DateTime", L"System.Generic.DateTime"},
        DefaultProperty{L"System.StructuredQueryType.Boolean", L"System.Generic.Boolean"},
        DefaultProperty{L"System.StructuredQueryType.FloatingPoint", L"System.Generic.FloatingPoint"}};
    for (const auto& item : defaults) {
        PropertyVariant value;
        if (FAILED(hr = InitPropVariantFromString(item.name, &value.value))) return hr;
        if (FAILED(hr = queryParser->SetMultiOption(SQMO_DEFAULT_PROPERTY, item.type, &value.value))) return hr;
    }
    return queryParser.CopyTo(result);
}

HRESULT fingerprint(ICondition* item, std::wstring& result, unsigned& count, unsigned depth);
HRESULT compoundFingerprints(ICondition* item, CONDITION_TYPE connective, std::vector<std::wstring>& result,
                             unsigned& count, unsigned depth) {
    if (depth > 64) return unsupported;
    CONDITION_TYPE type{};
    auto hr = item->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type != connective) {
        std::wstring text;
        if (FAILED(hr = fingerprint(item, text, count, depth))) return hr;
        result.push_back(std::move(text));
        return S_OK;
    }
    if (++count > maximumNodes) return unsupported;
    ComPtr<IEnumUnknown> enumeration;
    if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) return S_OK;
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> child;
        if (FAILED(hr = unknown.As(&child))) return hr;
        if (FAILED(hr = compoundFingerprints(child.Get(), connective, result, count, depth + 1))) return hr;
    }
}

// Use actual resolved scalar values, rather than parser input offsets encoded
// by native filename generators. The two trees use the same reference time.
HRESULT fingerprint(ICondition* item, std::wstring& result, unsigned& count, unsigned depth = 0) {
    if (depth > 64 || ++count > maximumNodes || result.size() > maximumBytes) return unsupported;
    CONDITION_TYPE type{};
    auto hr = item->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    auto append = [&](std::wstring_view value) { result += std::to_wstring(value.size()) + L":"; result += value; };
    result += std::to_wstring(type) + L"{";
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawProperty = nullptr;
        CONDITION_OPERATION op{};
        PropertyVariant value;
        hr = item->GetComparisonInfo(&rawProperty, &op, &value.value); TaskString propertyName(rawProperty);
        if (FAILED(hr)) return hr;
        std::wstring propertyText = rawProperty ? rawProperty : L"";
        std::transform(propertyText.begin(), propertyText.end(), propertyText.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        append(propertyText);
        result += std::to_wstring(op) + L":" + std::to_wstring(value.value.vt) + L":";
        switch (value.value.vt) {
        case VT_LPWSTR: if (!value.value.pwszVal) return unsupported; append(value.value.pwszVal); break;
        case VT_UI8: append(std::to_wstring(value.value.uhVal.QuadPart)); break;
        case VT_I8: append(std::to_wstring(value.value.hVal.QuadPart)); break;
        case VT_UI4: append(std::to_wstring(value.value.ulVal)); break;
        case VT_I4: append(std::to_wstring(value.value.lVal)); break;
        case VT_UI2: append(std::to_wstring(value.value.uiVal)); break;
        case VT_I2: append(std::to_wstring(value.value.iVal)); break;
        case VT_BOOL: append(value.value.boolVal == VARIANT_FALSE ? L"false" : L"true"); break;
        case VT_R8: append(std::to_wstring(std::bit_cast<uint64_t>(value.value.dblVal))); break;
        case VT_R4: append(std::to_wstring(std::bit_cast<uint32_t>(value.value.fltVal))); break;
        case VT_FILETIME:
            append(std::to_wstring((static_cast<ULONGLONG>(value.value.filetime.dwHighDateTime) << 32) |
                                  value.value.filetime.dwLowDateTime)); break;
        case VT_EMPTY: break;
        default: return unsupported;
        }
    } else if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&child)))) return hr;
        if (FAILED(hr = fingerprint(child.Get(), result, count, depth + 1))) return hr;
    } else if (type == CT_AND_CONDITION || type == CT_OR_CONDITION) {
        ComPtr<IEnumUnknown> enumeration;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
        std::vector<std::wstring> children;
        for (;;) {
            ComPtr<IUnknown> unknown;
            hr = enumeration->Next(1, &unknown, nullptr);
            if (hr == S_FALSE) break;
            if (FAILED(hr)) return hr;
            if (children.size() == maximumNodes) return unsupported;
            ComPtr<ICondition> child;
            if (FAILED(hr = unknown.As(&child))) return hr;
            if (FAILED(hr = compoundFingerprints(child.Get(), type, children, count, depth + 1))) return hr;
        }
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
        if (children.size() == 1) { result = children[0]; return S_OK; }
        for (const auto& child : children) append(child);
    } else return unsupported;
    result += L'}';
    return result.size() > maximumBytes ? unsupported : S_OK;
}

HRESULT plainTree(ICondition* item, IConditionFactory2* factory, ICondition** result, unsigned& count, unsigned depth = 0) {
    if (depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{};
    auto hr = item->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawProperty = nullptr; CONDITION_OPERATION op{}; PropertyVariant value;
        hr = item->GetComparisonInfo(&rawProperty, &op, &value.value); TaskString propertyName(rawProperty);
        if (FAILED(hr)) return hr;
        return factory->MakeLeaf(rawProperty, op, nullptr, &value.value, nullptr, nullptr, nullptr, FALSE, result);
    }
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child, converted;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&child)))) return hr;
        if (FAILED(hr = plainTree(child.Get(), factory, &converted, count, depth + 1))) return hr;
        return factory->MakeNot(converted.Get(), FALSE, result);
    }
    if (type != CT_AND_CONDITION && type != CT_OR_CONDITION) return unsupported;
    ComPtr<IEnumUnknown> enumeration;
    if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
    std::vector<ComPtr<ICondition>> owned; std::vector<ICondition*> raw;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> child, converted;
        if (FAILED(hr = unknown.As(&child))) return hr;
        if (FAILED(hr = plainTree(child.Get(), factory, &converted, count, depth + 1))) return hr;
        raw.push_back(converted.Get()); owned.push_back(converted);
    }
    return factory->CreateCompoundFromArray(type, raw.data(), static_cast<ULONG>(raw.size()),
                                           CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}

HRESULT restatableTree(ICondition* item, IConditionFactory2* factory, IConditionFactory* resolver,
                      const SYSTEMTIME& now, ICondition** result, unsigned& count, unsigned depth = 0) {
    if (depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{};
    auto hr = item->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        PWSTR raw = nullptr;
        hr = item->GetValueType(&raw); TaskString semantic(raw);
        if (FAILED(hr)) return hr;
        // Native filename lexical metadata has no public inverse API. Resolve
        // that leaf to actual property/comparison/scalar nodes before restating
        // it, while leaving relative date nodes unresolved for persistence.
        if (raw && wcscmp(raw, L"System.StructuredQueryType.FilePath") == 0) {
            ComPtr<ICondition> resolved;
            if (FAILED(hr = resolver->Resolve(item, SQRO_DONT_SPLIT_WORDS, &now, &resolved))) return hr;
            return plainTree(resolved.Get(), factory, result, count, depth + 1);
        }
        item->AddRef(); *result = item; return S_OK;
    }
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child, converted;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&child)))) return hr;
        if (FAILED(hr = restatableTree(child.Get(), factory, resolver, now, &converted, count, depth + 1))) return hr;
        return factory->MakeNot(converted.Get(), FALSE, result);
    }
    if (type != CT_AND_CONDITION && type != CT_OR_CONDITION) return unsupported;
    ComPtr<IEnumUnknown> enumeration;
    if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
    std::vector<ComPtr<ICondition>> owned;
    std::vector<ICondition*> raw;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> child, converted;
        if (FAILED(hr = unknown.As(&child))) return hr;
        if (FAILED(hr = restatableTree(child.Get(), factory, resolver, now, &converted, count, depth + 1))) return hr;
        raw.push_back(converted.Get()); owned.push_back(converted);
    }
    return factory->CreateCompoundFromArray(type, raw.data(), static_cast<ULONG>(raw.size()),
                                           CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}

HRESULT relativeTokens(ICondition* item, std::vector<std::wstring>& result, unsigned& count, unsigned depth = 0) {
    if (depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{};
    auto hr = item->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawSemantic = nullptr;
        hr = item->GetValueType(&rawSemantic); TaskString semantic(rawSemantic);
        if (FAILED(hr)) return hr;
        if (!rawSemantic || wcscmp(rawSemantic, L"System.StructuredQueryType.DateTime") != 0) return S_OK;
        PWSTR rawProperty = nullptr; CONDITION_OPERATION op{}; PropertyVariant value;
        hr = item->GetComparisonInfo(&rawProperty, &op, &value.value); TaskString propertyName(rawProperty);
        if (FAILED(hr)) return hr;
        if (value.value.vt != VT_LPWSTR || !value.value.pwszVal) return unsupported;
        std::wstring text = rawProperty ? rawProperty : L"";
        // Native parsing can push NOT through a compound range and invert
        // its comparisons. Preserve each unresolved date token here; the full
        // resolved-tree fingerprint below verifies the resulting operations
        // and Boolean meaning, rather than rejecting an equivalent negation.
        text += L"\n"; text += value.value.pwszVal;
        result.push_back(std::move(text));
        return S_OK;
    }
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&child)))) return hr;
        return relativeTokens(child.Get(), result, count, depth + 1);
    }
    if (type != CT_AND_CONDITION && type != CT_OR_CONDITION) return unsupported;
    ComPtr<IEnumUnknown> enumeration;
    if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) return S_OK;
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> child;
        if (FAILED(hr = unknown.As(&child))) return hr;
        if (FAILED(hr = relativeTokens(child.Get(), result, count, depth + 1))) return hr;
    }
}

HRESULT canonicalUtcSecond(IQueryParser* parser, const wchar_t* property, CONDITION_OPERATION operation,
                           const PROPVARIANT& value, std::wstring& result) {
    // Restatement of a constructed UTC leaf can omit its time-zone suffix.
    // Derive a candidate only through public date resolution, then require the
    // parser to reproduce the exact original unresolved token. This never
    // decodes the token or changes a relative date into an absolute timestamp.
    const wchar_t* symbol = nullptr;
    switch (operation) {
    case COP_IMPLICIT: symbol = L""; break;
    case COP_EQUAL: symbol = L"="; break;
    case COP_NOTEQUAL: symbol = L"<>"; break;
    case COP_LESSTHAN: symbol = L"<"; break;
    case COP_GREATERTHAN: symbol = L">"; break;
    case COP_LESSTHANOREQUAL: symbol = L"<="; break;
    case COP_GREATERTHANOREQUAL: symbol = L">="; break;
    default: return S_FALSE;
    }
    ComPtr<IConditionFactory> factory;
    auto hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<ICondition> equality, resolved;
    if (FAILED(hr = factory->MakeLeaf(property, COP_EQUAL, L"System.StructuredQueryType.DateTime", &value,
                                    nullptr, nullptr, nullptr, FALSE, &equality))) return hr;
    SYSTEMTIME now{}; GetLocalTime(&now);
    if (FAILED(hr = factory->Resolve(equality.Get(), SQRO_DONT_SPLIT_WORDS, &now, &resolved))) return hr;
    CONDITION_TYPE type{};
    if (!resolved || FAILED(hr = resolved->GetConditionType(&type))) return FAILED(hr) ? hr : E_UNEXPECTED;
    if (type != CT_AND_CONDITION) return S_FALSE;
    ComPtr<IEnumUnknown> children;
    if (FAILED(hr = resolved->GetSubConditions(IID_PPV_ARGS(&children)))) return hr;
    ULONGLONG first = 0, last = 0;
    bool haveFirst = false, haveLast = false;
    for (unsigned index = 0; index != 3; ++index) {
        ComPtr<IUnknown> unknown;
        hr = children->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) { if (index != 2) return S_FALSE; break; }
        if (FAILED(hr)) return hr;
        if (index == 2) return S_FALSE;
        ComPtr<ICondition> child;
        if (FAILED(hr = unknown.As(&child))) return hr;
        PWSTR rawProperty = nullptr; CONDITION_OPERATION comparison{}; PropertyVariant boundary;
        hr = child->GetComparisonInfo(&rawProperty, &comparison, &boundary.value); TaskString name(rawProperty);
        if (FAILED(hr)) return hr;
        if (!rawProperty || _wcsicmp(rawProperty, property) != 0 || boundary.value.vt != VT_FILETIME) return S_FALSE;
        const auto ticks = (static_cast<ULONGLONG>(boundary.value.filetime.dwHighDateTime) << 32) |
                          boundary.value.filetime.dwLowDateTime;
        if (comparison == COP_GREATERTHANOREQUAL && !haveFirst) { first = ticks; haveFirst = true; }
        else if (comparison == COP_LESSTHAN && !haveLast) { last = ticks; haveLast = true; }
        else return S_FALSE;
    }
    constexpr ULONGLONG second = 10000000;
    if (!haveFirst || !haveLast || last < first || last - first != second || first % second) return S_FALSE;
    const FILETIME instant{static_cast<DWORD>(first), static_cast<DWORD>(first >> 32)};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&instant, &utc) || utc.wYear > 9999) return S_FALSE;
    wchar_t iso[40]{};
    swprintf_s(iso, L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay,
               utc.wHour, utc.wMinute, utc.wSecond);
    std::wstring candidate = property;
    candidate += L':'; candidate += symbol; candidate += iso;
    ComPtr<IQuerySolution> solution;
    if (FAILED(hr = parser->Parse(candidate.c_str(), nullptr, &solution))) return hr;
    ComPtr<ICondition> reparsed;
    if (FAILED(hr = solution->GetQuery(&reparsed, nullptr))) return hr;
    if (FAILED(hr = reparsed->GetConditionType(&type))) return hr;
    if (type != CT_LEAF_CONDITION) return S_FALSE;
    PWSTR rawProperty = nullptr, rawType = nullptr; CONDITION_OPERATION checkedOperation{}; PropertyVariant checked;
    hr = reparsed->GetComparisonInfo(&rawProperty, &checkedOperation, &checked.value); TaskString name(rawProperty);
    if (FAILED(hr)) return hr;
    hr = reparsed->GetValueType(&rawType); TaskString semantic(rawType);
    if (FAILED(hr)) return hr;
    if (!rawProperty || _wcsicmp(rawProperty, property) != 0 || checkedOperation != operation ||
        !rawType || wcscmp(rawType, L"System.StructuredQueryType.DateTime") != 0 || checked.value.vt != value.vt ||
        PropVariantCompareEx(value, checked.value, PVCU_DEFAULT, PVCF_DEFAULT) != 0) return S_FALSE;
    result += candidate;
    return S_OK;
}

// Restate each leaf through the native schema, then spell out Boolean grouping.
// Whole-tree restatement can factor one property around a group in a form that
// does not reparse faithfully for filename generators; the equivalence check
// below still validates the complete output before it becomes editable state.
HRESULT queryString(IQueryParser* parser, ICondition* item, std::wstring& result, unsigned& count,
                    bool canonicalLiterals = false, unsigned depth = 0) {
    if (depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{};
    auto hr = item->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawProperty = nullptr, rawType = nullptr;
        CONDITION_OPERATION op{}; PropertyVariant value;
        hr = item->GetComparisonInfo(&rawProperty, &op, &value.value); TaskString propertyName(rawProperty);
        if (FAILED(hr)) return hr;
        hr = item->GetValueType(&rawType); TaskString semantic(rawType);
        if (FAILED(hr)) return hr;
        if (rawProperty && rawType && wcscmp(rawType, L"System.StructuredQueryType.DateTime") == 0 &&
            value.value.vt == VT_LPWSTR && value.value.pwszVal) {
            hr = canonicalUtcSecond(parser, rawProperty, op, value.value, result);
            if (hr != S_FALSE) return hr;
        }
        if (canonicalLiterals && rawProperty && value.value.vt == VT_LPWSTR && value.value.pwszVal &&
            (!rawType || wcscmp(rawType, L"System.StructuredQueryType.String") == 0 ||
                         wcscmp(rawType, L"System.StructuredQueryType.Blurb") == 0)) {
            // Public canonical AQS permits quoted literal strings, with every
            // embedded quote doubled. Use it when native restatement loses
            // literal quoting. Structured date/number/FilePath encodings never
            // enter this branch; the full semantic comparison is still required.
            // https://learn.microsoft.com/en-us/windows/win32/search/-search-3x-advancedquerysyntax
            const wchar_t* symbol = nullptr;
            switch (op) {
            case COP_EQUAL: symbol = L"="; break;
            case COP_NOTEQUAL: symbol = L"<>"; break;
            case COP_LESSTHAN: symbol = L"<"; break;
            case COP_GREATERTHAN: symbol = L">"; break;
            case COP_LESSTHANOREQUAL: symbol = L"<="; break;
            case COP_GREATERTHANOREQUAL: symbol = L">="; break;
            case COP_VALUE_STARTSWITH: symbol = L"~<"; break;
            case COP_VALUE_ENDSWITH: symbol = L"~>"; break;
            case COP_VALUE_CONTAINS: symbol = L"~="; break;
            case COP_VALUE_NOTCONTAINS: symbol = L"~!"; break;
            case COP_DOSWILDCARDS: symbol = L"~"; break;
            case COP_WORD_EQUAL: symbol = L"$="; break;
            case COP_WORD_STARTSWITH: symbol = L"$<"; break;
            default: break;
            }
            if (symbol) {
                result += rawProperty; result += L':'; result += symbol; result += L'\"';
                for (const auto* p = value.value.pwszVal; *p; ++p) {
                    if (*p == L'\"') result += L'\"';
                    result += *p;
                }
                result += L'\"';
                return result.size() > maximumQueryLength ? unsupported : S_OK;
            }
        }
        ComPtr<ICondition> positive;
        const bool negateContains = op == COP_VALUE_NOTCONTAINS;
        if (negateContains) {
            // Spell the documented inverse as NOT(contains). Native direct
            // restatement of a not-contains leaf can emit both NOT and ~!,
            // which reparses as contains; full semantic validation follows.
            ComPtr<IConditionFactory> factory;
            if (FAILED(hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return hr;
            if (FAILED(hr = factory->MakeLeaf(rawProperty, COP_VALUE_CONTAINS, rawType, &value.value,
                                             nullptr, nullptr, nullptr, FALSE, &positive))) return hr;
        }
        PWSTR raw = nullptr;
        hr = parser->RestateToString(positive ? positive.Get() : item, FALSE, &raw); TaskString text(raw);
        if (FAILED(hr)) return hr;
        if (!raw || !*raw) return unsupported;
        if (negateContains) result += L"NOT (";
        result += raw;
        if (negateContains) result += L')';
    } else if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&child)))) return hr;
        result += L"NOT (";
        if (FAILED(hr = queryString(parser, child.Get(), result, count, canonicalLiterals, depth + 1))) return hr;
        result += L')';
    } else if (type == CT_AND_CONDITION || type == CT_OR_CONDITION) {
        ComPtr<IEnumUnknown> enumeration;
        if (FAILED(hr = item->GetSubConditions(IID_PPV_ARGS(&enumeration)))) return hr;
        unsigned children = 0;
        for (;;) {
            ComPtr<IUnknown> unknown;
            hr = enumeration->Next(1, &unknown, nullptr);
            if (hr == S_FALSE) break;
            if (FAILED(hr)) return hr;
            ComPtr<ICondition> child;
            if (FAILED(hr = unknown.As(&child))) return hr;
            if (children++) result += type == CT_AND_CONDITION ? L" AND " : L" OR ";
            result += L'(';
            if (FAILED(hr = queryString(parser, child.Get(), result, count, canonicalLiterals, depth + 1))) return hr;
            result += L')';
        }
        if (!children) return unsupported;
    } else return unsupported;
    return result.size() > maximumQueryLength ? unsupported : S_OK;
}

HRESULT restatedQuery(IQueryParser* queryParser, ICondition* original, ICondition* candidate,
                      const SYSTEMTIME& now, std::wstring& result, bool canonicalLiterals = false) {
    std::wstring text;
    unsigned restatementCount = 0;
    auto hr = queryString(queryParser, candidate, text, restatementCount, canonicalLiterals);
    if (FAILED(hr)) return hr;
    ComPtr<IQuerySolution> solution;
    if (FAILED(hr = queryParser->Parse(text.c_str(), nullptr, &solution))) return hr;
    ComPtr<ICondition> reparsed;
    if (FAILED(hr = solution->GetQuery(&reparsed, nullptr))) return hr;
    unsigned count = 0;
    std::vector<std::wstring> expectedDates, actualDates;
    if (FAILED(hr = relativeTokens(original, expectedDates, count))) return hr;
    count = 0;
    if (FAILED(hr = relativeTokens(reparsed.Get(), actualDates, count))) return hr;
    std::sort(expectedDates.begin(), expectedDates.end()); std::sort(actualDates.begin(), actualDates.end());
    if (expectedDates != actualDates) return unsupported;
    ComPtr<ICondition> expectedTree, actualTree;
    if (FAILED(hr = solution->Resolve(original, SQRO_DONT_SPLIT_WORDS, &now, &expectedTree)) ||
        FAILED(hr = solution->Resolve(reparsed.Get(), SQRO_DONT_SPLIT_WORDS, &now, &actualTree))) return hr;
    std::wstring expected, actual;
    count = 0;
    if (FAILED(hr = fingerprint(expectedTree.Get(), expected, count, 0))) return hr;
    count = 0;
    if (FAILED(hr = fingerprint(actualTree.Get(), actual, count, 0))) return hr;
    if (expected != actual) return unsupported;
    result = std::move(text);
    return S_OK;
}

HRESULT queryMetadata(IXMLDOMNode* node, SavedSearchMetadata& result) {
    Attributes values;
    auto hr = attributes(node, {}, values);
    if (FAILED(hr)) return hr;
    Elements children;
    if (FAILED(hr = elements(node, children))) return hr;
    ComPtr<IXMLDOMNode> scope, conditions, kinds;
    for (const auto& child : children) {
        std::wstring name;
        if (FAILED(hr = nodeName(child.Get(), name))) return hr;
        auto* slot = name == L"scope" ? std::addressof(scope) : name == L"conditions" ? std::addressof(conditions) :
                     name == L"kindList" ? std::addressof(kinds) : nullptr;
        if (!slot || *slot) return unsupported;
        *slot = child;
    }
    if (!scope || !conditions || !kinds) return unsupported;
    bool needsGuard = false;
    if (FAILED(hr = scopeMetadata(scope.Get(), result, needsGuard))) return hr;
    values.clear();
    if (FAILED(hr = attributes(kinds.Get(), {}, values))) return hr;
    Elements kindChildren;
    if (FAILED(hr = elements(kinds.Get(), kindChildren))) return hr;
    if (kindChildren.empty() || kindChildren.size() > 64) return unsupported;
    constexpr std::array<std::wstring_view, 23> supportedKinds{
        L"calendar", L"communication", L"contact", L"document", L"email", L"feed", L"folder", L"game",
        L"instantmessage", L"journal", L"link", L"movie", L"music", L"note", L"picture", L"program",
        L"recordedtv", L"searchfolder", L"task", L"video", L"webhistory", L"item", L"other"};
    std::vector<std::wstring> kindNames;
    for (const auto& child : kindChildren) {
        std::wstring name;
        if (FAILED(hr = nodeName(child.Get(), name))) return hr;
        values.clear();
        if (name != L"kind" || FAILED(hr = attributes(child.Get(), {L"name"}, values))) return name == L"kind" ? hr : unsupported;
        if (values.size() != 1 || FAILED(hr = emptyElement(child.Get()))) return values.size() == 1 ? hr : unsupported;
        auto kind = values[L"name"];
        std::transform(kind.begin(), kind.end(), kind.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (std::find(supportedKinds.begin(), supportedKinds.end(), kind) == supportedKinds.end()) return unsupported;
        kindNames.push_back(std::move(kind));
    }
    // Native all-item is a singleton loader sentinel. Repeating it produces
    // an empty union rather than an all-item query; retain that external shape
    // in the native viewer instead of simplifying it to a different search.
    if (kindNames.size() > 1 && std::all_of(kindNames.begin(), kindNames.end(), [](const std::wstring& kind) { return kind == L"item"; }))
        return unsupported;
    values.clear();
    if (FAILED(hr = attributes(conditions.Get(), {}, values))) return hr;
    Elements roots;
    if (FAILED(hr = elements(conditions.Get(), roots))) return hr;
    if (roots.size() != 1) return unsupported;
    ComPtr<IConditionFactory2> factory;
    if (FAILED(hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return hr;
    ComPtr<IQueryParser> queryParser;
    if (FAILED(hr = parser(&queryParser))) return hr;
    ComPtr<ICondition> tree;
    if (FAILED(hr = condition(roots[0].Get(), factory.Get(), queryParser.Get(), &tree))) return hr;
    if (needsGuard) {
        ComPtr<ICondition> query;
        if (FAILED(hr = removeProtectiveScopeGuard(tree.Get(), result.scopeRules, &query))) return hr;
        tree = std::move(query);
    }
    if (std::any_of(kindNames.begin(), kindNames.end(), [](const std::wstring& kind) { return kind != L"item"; })) {
        // Keep System.Kind typed while constructing the union. A quoted
        // textual System.Kind value can be parsed as a generic search term;
        // RestateToString below supplies the native canonical entity syntax
        // and verifies the parser round-trip against these actual values.
        std::vector<ComPtr<ICondition>> owned;
        std::vector<ICondition*> branches;
        for (const auto& kind : kindNames) {
            if (kind == L"item") continue;
            PropertyVariant value;
            if (FAILED(hr = InitPropVariantFromString(kind.c_str(), &value.value))) return hr;
            ComPtr<ICondition> leaf;
            if (FAILED(hr = factory->MakeLeaf(L"System.Kind", COP_EQUAL, nullptr, &value.value,
                nullptr, nullptr, nullptr, FALSE, &leaf))) return hr;
            branches.push_back(leaf.Get()); owned.push_back(std::move(leaf));
        }
        ComPtr<ICondition> restriction;
        if (branches.size() == 1) restriction = owned.front();
        else if (FAILED(hr = factory->CreateCompoundFromArray(CT_OR_CONDITION, branches.data(),
            static_cast<ULONG>(branches.size()), CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&restriction)))) return hr;
        ICondition* conjunction[]{tree.Get(), restriction.Get()};
        ComPtr<ICondition> combined;
        if (FAILED(hr = factory->CreateCompoundFromArray(CT_AND_CONDITION, conjunction, 2,
            CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&combined)))) return hr;
        tree = std::move(combined);
    }
    SYSTEMTIME now{}; GetLocalTime(&now);
    hr = restatedQuery(queryParser.Get(), tree.Get(), tree.Get(), now, result.query);
    if (hr != unsupported) return hr;
    ComPtr<IQuerySolution> resolver;
    if (FAILED(hr = queryParser->Parse(L"System.Size:>=0", nullptr, &resolver))) return hr;
    ComPtr<ICondition> restatable;
    unsigned conversionCount = 0;
    if (FAILED(hr = restatableTree(tree.Get(), factory.Get(), resolver.Get(), now, &restatable, conversionCount))) return hr;
    return restatedQuery(queryParser.Get(), tree.Get(), restatable.Get(), now, result.query, true);
}
} // namespace

bool saved_search_internal::supportedValueType(std::wstring_view type) noexcept {
    constexpr std::array<std::wstring_view, 8> supportedTypes{
        L"System.StructuredQueryType.String",
        L"System.StructuredQueryType.Integer", L"System.StructuredQueryType.FloatingPoint",
        L"System.StructuredQueryType.Boolean", L"System.StructuredQueryType.DateTime",
        L"System.StructuredQueryType.FilePath", L"System.StructuredQueryType.Implicit.System.Kind",
        L"System.StructuredQueryType.Implicit.System.Size"};
    return std::find(supportedTypes.begin(), supportedTypes.end(), type) != supportedTypes.end();
}

HRESULT saved_search_internal::validateSerializedLimits(std::string_view xml) {
    if (xml.empty()) return invalidData;
    if (xml.size() > maximumBytes) return unsupported;
    ComPtr<IXMLDOMDocument2> document;
    auto hr = parseBytes(xml, &document);
    if (FAILED(hr)) return hr;
    unsigned count = 0;
    return boundedTree(document.Get(), 0, count);
}

HRESULT readSavedSearch(const std::filesystem::path& path, SavedSearchMetadata* result) {
    if (!result) return E_POINTER;
    try {
        if (path.empty() || path.native().find(L'\0') != std::wstring::npos || _wcsicmp(path.extension().c_str(), L".search-ms") != 0)
            return E_INVALIDARG;
        ComPtr<IXMLDOMDocument2> document;
        std::string bytes;
        auto hr = loadDocument(path, &document, bytes);
        if (FAILED(hr)) return hr;
        unsigned count = 0;
        if (FAILED(hr = boundedTree(document.Get(), 0, count))) return hr;
        ComPtr<IXMLDOMElement> root;
        if (FAILED(hr = document->get_documentElement(&root))) return hr;
        if (!root) return invalidData;
        std::wstring name;
        if (FAILED(hr = nodeName(root.Get(), name))) return hr;
        if (name != L"persistedQuery") return unsupported;
        Attributes values;
        if (FAILED(hr = attributes(root.Get(), {L"version"}, values))) return hr;
        if (values.size() != 1 || values[L"version"] != L"1.0") return unsupported;
        Elements children;
        if (FAILED(hr = elements(root.Get(), children))) return hr;
        ComPtr<IXMLDOMNode> query, view, properties;
        for (const auto& child : children) {
            if (FAILED(hr = nodeName(child.Get(), name))) return hr;
            if (name == L"query" && !query) query = child;
            else if (name == L"viewInfo" && !view) view = child;
            else if (name == L"properties" && !properties) {
                properties = child;
            }
            else return unsupported;
        }
        if (!query) return unsupported;
        SavedSearchMetadata candidate;
        if (properties) {
            candidate.fileProperties.emplace();
            if (FAILED(hr = filePropertyMetadata(properties.Get(), *candidate.fileProperties))) return hr;
            if (FAILED(hr = exactFilePropertyText(bytes, *candidate.fileProperties))) return hr;
        }
        if (view) {
            candidate.presentation.emplace();
            if (FAILED(hr = viewMetadata(view.Get(), *candidate.presentation))) return hr;
        }
        if (FAILED(hr = queryMetadata(query.Get(), candidate))) return hr;
        *result = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error&) { return E_INVALIDARG; }
}
} // namespace explorer
