#include "explorer/saved_search.hpp"
#include <msxml6.h>
#include <structuredquery.h>
#include <shlobj.h>
#include <propvarutil.h>
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

HRESULT loadDocument(const std::filesystem::path& path, IXMLDOMDocument2** result) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) return HRESULT_FROM_WIN32(GetLastError());
    if (size.QuadPart <= 0) return invalidData;
    if (size.QuadPart > static_cast<LONGLONG>(maximumBytes)) return unsupported;
    Variant source;
    source.value.vt = VT_ARRAY | VT_UI1;
    source.value.parray = SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(size.QuadPart));
    if (!source.value.parray) return E_OUTOFMEMORY;
    void* buffer = nullptr;
    auto hr = SafeArrayAccessData(source.value.parray, &buffer);
    if (FAILED(hr)) return hr;
    DWORD received = 0;
    const bool read = ReadFile(file.get(), buffer, static_cast<DWORD>(size.QuadPart), &received, nullptr) != FALSE;
    const auto error = GetLastError();
    SafeArrayUnaccessData(source.value.parray);
    if (!read) return HRESULT_FROM_WIN32(error);
    if (received != static_cast<DWORD>(size.QuadPart)) return invalidData;

    ComPtr<IXMLDOMDocument2> document;
    hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = document->put_async(VARIANT_FALSE)) ||
        FAILED(hr = document->put_validateOnParse(VARIANT_FALSE)) ||
        FAILED(hr = document->put_resolveExternals(VARIANT_FALSE))) return hr;
    VARIANT option{}; option.vt = VT_BOOL; option.boolVal = VARIANT_TRUE;
    if (FAILED(hr = property(document.Get(), L"ProhibitDTD", option))) return hr;
    option.boolVal = VARIANT_FALSE;
    if (FAILED(hr = property(document.Get(), L"UseInlineSchema", option))) return hr;
    option.vt = VT_I4; option.lVal = maximumDepth;
    if (FAILED(hr = property(document.Get(), L"MaxElementDepth", option))) return hr;
    VARIANT_BOOL loaded = VARIANT_FALSE;
    hr = document->load(source.value, &loaded);
    if (FAILED(hr)) return hr;
    if (loaded != VARIANT_TRUE) return invalidData;
    return document.CopyTo(result);
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

HRESULT scopeMetadata(IXMLDOMNode* node, SavedSearchMetadata& result) {
    Attributes scopeAttributes;
    auto hr = attributes(node, {}, scopeAttributes);
    if (FAILED(hr)) return hr;
    Elements children;
    if (FAILED(hr = elements(node, children))) return hr;
    if (children.size() != 1) return unsupported;
    std::wstring name;
    if (FAILED(hr = nodeName(children[0].Get(), name))) return hr;
    if (name != L"include") return unsupported;
    Attributes values;
    if (FAILED(hr = attributes(children[0].Get(), {L"path", L"knownFolder", L"nonRecursive"}, values))) return hr;
    if (FAILED(hr = emptyElement(children[0].Get()))) return hr;
    const auto path = values.find(L"path"), known = values.find(L"knownFolder");
    if ((path == values.end()) == (known == values.end())) return unsupported;
    const auto recursion = values.find(L"nonRecursive");
    if (recursion != values.end()) {
        if (recursion->second == L"true") result.recursive = false;
        else if (recursion->second != L"false") return unsupported;
    }
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
        // Match the one supported documented GUID directly. CLSIDFromString
        // also accepts ProgIDs and can register unknown ones; no registry lookup
        // or conversion of arbitrary XML text is necessary for this subset.
        wchar_t computerIdentifier[40]{};
        if (!StringFromGUID2(FOLDERID_ComputerFolder, computerIdentifier, 40)) return E_UNEXPECTED;
        if (_wcsicmp(known->second.c_str(), computerIdentifier) != 0 || !result.recursive) return unsupported;
        if (FAILED(hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&result.scope)))) return hr;
    }
    SFGAOF flags = 0;
    if (FAILED(hr = result.scope->GetAttributes(SFGAO_FOLDER, &flags))) return hr;
    return flags & SFGAO_FOLDER ? S_OK : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
}

bool operation(std::wstring_view name, CONDITION_OPERATION& result) {
    struct Mapping { std::wstring_view name; CONDITION_OPERATION operation; };
    constexpr std::array mappings{
        Mapping{L"imp", COP_IMPLICIT}, Mapping{L"eq", COP_EQUAL}, Mapping{L"neq", COP_NOTEQUAL},
        Mapping{L"lt", COP_LESSTHAN}, Mapping{L"gt", COP_GREATERTHAN}, Mapping{L"lte", COP_LESSTHANOREQUAL},
        Mapping{L"gte", COP_GREATERTHANOREQUAL}, Mapping{L"starts with", COP_VALUE_STARTSWITH},
        Mapping{L"ends with", COP_VALUE_ENDSWITH}, Mapping{L"contains", COP_VALUE_CONTAINS},
        Mapping{L"does not contain", COP_VALUE_NOTCONTAINS}, Mapping{L"matches", COP_DOSWILDCARDS},
        Mapping{L"wordmatch", COP_WORD_EQUAL}};
    for (const auto& mapping : mappings) if (name == mapping.name) { result = mapping.operation; return true; }
    return false;
}

HRESULT condition(IXMLDOMNode* node, IConditionFactory2* factory, ICondition** result, unsigned depth = 0) {
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
        if (propertyType == values.end() || op == values.end() || value == values.end()) return invalidData;
        if (propertyType->second != L"wstr" && propertyType->second != L"string") return unsupported;
        CONDITION_OPERATION comparison{};
        if (!operation(op->second, comparison)) return unsupported;
        const auto propertyName = values.find(L"property");
        if (propertyName == values.end() || propertyName->second.empty() || propertyName->second.size() > 1024) return unsupported;
        auto semantic = values.find(L"valuetype");
        const auto otherCase = values.find(L"valueType");
        if (semantic != values.end() && otherCase != values.end()) return unsupported;
        if (semantic == values.end()) semantic = otherCase;
        if (semantic != values.end() && (semantic->second.empty() || semantic->second.size() > 1024)) return unsupported;
        if (semantic != values.end()) {
            constexpr std::array<std::wstring_view, 8> supportedTypes{
                L"System.StructuredQueryType.String",
                L"System.StructuredQueryType.Integer", L"System.StructuredQueryType.FloatingPoint",
                L"System.StructuredQueryType.Boolean", L"System.StructuredQueryType.DateTime",
                L"System.StructuredQueryType.FilePath", L"System.StructuredQueryType.Implicit.System.Kind",
                L"System.StructuredQueryType.Implicit.System.Size"};
            if (std::find(supportedTypes.begin(), supportedTypes.end(), semantic->second) == supportedTypes.end()) return unsupported;
        }
        if (value->second.size() > maximumQueryLength) return unsupported;
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
        if (FAILED(hr = condition(children[0].Get(), factory, &child, depth + 1))) return hr;
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
        if (FAILED(hr = condition(child.Get(), factory, &item, depth + 1))) return hr;
        raw.push_back(item.Get()); owned.push_back(item);
    }
    return factory->CreateCompoundFromArray(connective, raw.data(), static_cast<ULONG>(raw.size()),
                                           CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
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
        text += L"\n" + std::to_wstring(op) + L"\n" + value.value.pwszVal;
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
            case COP_WORD_STARTSWITH:
                if (_wcsicmp(rawProperty, L"System.FileName") == 0) return unsupported;
                symbol = L"$<"; break;
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
    if (FAILED(hr = scopeMetadata(scope.Get(), result))) return hr;
    values.clear();
    if (FAILED(hr = attributes(kinds.Get(), {}, values))) return hr;
    Elements kindChildren;
    if (FAILED(hr = elements(kinds.Get(), kindChildren))) return hr;
    if (kindChildren.size() != 1) return unsupported;
    std::wstring name;
    if (FAILED(hr = nodeName(kindChildren[0].Get(), name))) return hr;
    values.clear();
    if (name != L"kind" || FAILED(hr = attributes(kindChildren[0].Get(), {L"name"}, values))) return name == L"kind" ? hr : unsupported;
    if (values.size() != 1 || values[L"name"] != L"item") return unsupported;
    if (FAILED(hr = emptyElement(kindChildren[0].Get()))) return hr;
    values.clear();
    if (FAILED(hr = attributes(conditions.Get(), {}, values))) return hr;
    Elements roots;
    if (FAILED(hr = elements(conditions.Get(), roots))) return hr;
    if (roots.size() != 1) return unsupported;
    ComPtr<IConditionFactory2> factory;
    if (FAILED(hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return hr;
    ComPtr<ICondition> tree;
    if (FAILED(hr = condition(roots[0].Get(), factory.Get(), &tree))) return hr;
    ComPtr<IQueryParser> queryParser;
    if (FAILED(hr = parser(&queryParser))) return hr;
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

HRESULT readSavedSearch(const std::filesystem::path& path, SavedSearchMetadata* result) {
    if (!result) return E_POINTER;
    try {
        if (path.empty() || path.native().find(L'\0') != std::wstring::npos || _wcsicmp(path.extension().c_str(), L".search-ms") != 0)
            return E_INVALIDARG;
        ComPtr<IXMLDOMDocument2> document;
        auto hr = loadDocument(path, &document);
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
        ComPtr<IXMLDOMNode> query;
        bool view = false, properties = false;
        for (const auto& child : children) {
            if (FAILED(hr = nodeName(child.Get(), name))) return hr;
            if (name == L"query" && !query) query = child;
            else if (name == L"viewInfo" && !view) view = true;
            else if (name == L"properties" && !properties) properties = true;
            else return unsupported;
        }
        if (!query) return unsupported;
        SavedSearchMetadata candidate;
        if (FAILED(hr = queryMetadata(query.Get(), candidate))) return hr;
        *result = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error&) { return E_INVALIDARG; }
}
} // namespace explorer
