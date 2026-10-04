#include "explorer/search.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <structuredquery.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <memory>
#include <new>
#include <string_view>
#include <limits>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;

constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
constexpr size_t maximumQueryLength = 32768;

struct TaskMemoryFree {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};
using TaskString = std::unique_ptr<wchar_t, TaskMemoryFree>;
struct Variant {
    PROPVARIANT value{};
    ~Variant() { PropVariantClear(&value); }
};

HRESULT queryText(const std::wstring& query, std::wstring& text) {
    if (query.size() > maximumQueryLength || query.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    if (!query.empty() && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, query.data(),
                                              static_cast<int>(query.size()), nullptr, 0, nullptr, nullptr))
        return E_INVALIDARG;
    const auto first = query.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return E_INVALIDARG;
    const auto last = query.find_last_not_of(L" \t\r\n");
    text = query.substr(first, last - first + 1);
    return S_OK;
}

HRESULT validateLiveCondition(ICondition* condition, unsigned depth, unsigned& count) {
    if (!condition || depth > 64 || ++count > 4096) return unsupported;
    CONDITION_TYPE type{};
    auto hr = condition->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawProperty = nullptr;
        CONDITION_OPERATION operation{};
        Variant value;
        hr = condition->GetComparisonInfo(&rawProperty, &operation, &value.value);
        TaskString property(rawProperty);
        if (FAILED(hr)) return hr;
        // Explicit filename word-prefix conditions remain unsupported until
        // live and saved native fixture compatibility is established. Inspect
        // the AST so quoted "$<" text and implicit/default terms still work.
        return operation == COP_WORD_STARTSWITH && rawProperty &&
               _wcsicmp(rawProperty, L"System.FileName") == 0 ? unsupported : S_OK;
    }
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        hr = condition->GetSubConditions(IID_PPV_ARGS(&child));
        return FAILED(hr) ? hr : validateLiveCondition(child.Get(), depth + 1, count);
    }
    if (type != CT_AND_CONDITION && type != CT_OR_CONDITION) return unsupported;
    ComPtr<IEnumUnknown> children;
    hr = condition->GetSubConditions(IID_PPV_ARGS(&children));
    if (FAILED(hr)) return hr;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = children->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) return S_OK;
        if (FAILED(hr)) return hr;
        if (!unknown) return E_UNEXPECTED;
        ComPtr<ICondition> child;
        hr = unknown.As(&child);
        if (FAILED(hr)) return hr;
        hr = validateLiveCondition(child.Get(), depth + 1, count);
        if (FAILED(hr)) return hr;
    }
}

HRESULT parseQuery(const std::wstring& text, bool resolve, ICondition** result) {
    ComPtr<IQueryParserManager> manager;
    auto hr = CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;

    ComPtr<IQueryParser> parser;
    hr = manager->CreateLoadedParser(L"SystemIndex", GetUserDefaultUILanguage(), IID_PPV_ARGS(&parser));
    if (FAILED(hr)) return hr;
    hr = manager->InitializeOptions(FALSE, TRUE, parser.Get());
    if (FAILED(hr)) return hr;

    // Unqualified terms must remain generic searches instead of being limited
    // to one metadata property. These are the SystemIndex schema's value types.
    struct DefaultProperty { const wchar_t* type; const wchar_t* name; };
    constexpr std::array defaults{
        DefaultProperty{L"System.StructuredQueryType.String", L"System.Generic.String"},
        DefaultProperty{L"System.StructuredQueryType.Integer", L"System.Generic.Integer"},
        DefaultProperty{L"System.StructuredQueryType.DateTime", L"System.Generic.DateTime"},
        DefaultProperty{L"System.StructuredQueryType.Boolean", L"System.Generic.Boolean"},
        DefaultProperty{L"System.StructuredQueryType.FloatingPoint", L"System.Generic.FloatingPoint"}};
    for (const auto& property : defaults) {
        PROPVARIANT value{};
        hr = InitPropVariantFromString(property.name, &value);
        if (SUCCEEDED(hr)) hr = parser->SetMultiOption(SQMO_DEFAULT_PROPERTY, property.type, &value);
        PropVariantClear(&value);
        if (FAILED(hr)) return hr;
    }

    ComPtr<IQuerySolution> solution;
    hr = parser->Parse(text.c_str(), nullptr, &solution);
    if (FAILED(hr)) return hr;
    ComPtr<ICondition> parsed;
    hr = solution->GetQuery(&parsed, nullptr);
    if (FAILED(hr)) return hr;
    unsigned conditionCount = 0;
    hr = validateLiveCondition(parsed.Get(), 0, conditionCount);
    if (FAILED(hr)) return hr;

    // Persistence must keep structured relative-date expressions unresolved.
    if (!resolve) return parsed.CopyTo(result);

    // Relative dates and virtual properties need resolution before execution.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return solution->Resolve(parsed.Get(), SQRO_DONT_SPLIT_WORDS, &now, result);
}

HRESULT validatedScope(IShellItem* scope, IShellItem** result) {
    ComPtr<IShellItem> item = scope;
    auto hr = S_OK;
    if (!item) hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    SFGAOF attributes = 0;
    hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
    if (FAILED(hr)) return hr;
    if (!(attributes & SFGAO_FOLDER)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    return item.CopyTo(result);
}

HRESULT filesystemScope(IShellItem* scope, std::wstring& path) {
    PWSTR raw = nullptr;
    const auto hr = scope->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    TaskString allocated(raw);
    if (FAILED(hr) || !raw || !*raw) return unsupported;
    path = raw;
    // ItemFolderPathDisplay has no trailing separator, except at a drive root.
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return S_OK;
}

HRESULT restrictToFolder(ICondition* query, const std::wstring& path, ICondition** result) {
    ComPtr<IConditionFactory2> factory;
    auto hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    Variant value;
    hr = InitPropVariantFromString(path.c_str(), &value.value);
    if (FAILED(hr)) return hr;
    ComPtr<ICondition> restriction;
    hr = factory->MakeLeaf(L"System.ItemFolderPathDisplay", COP_EQUAL, nullptr,
                           &value.value, nullptr, nullptr, nullptr, FALSE, &restriction);
    if (FAILED(hr)) return hr;
    ICondition* children[]{query, restriction.Get()};
    return factory->CreateCompoundFromArray(CT_AND_CONDITION, children, 2,
            CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}

// XML 1.0 attributes: reject illegal controls/UTF-16, and escape whitespace so
// XML attribute normalization cannot change a query's literal text.
HRESULT xmlAttribute(std::wstring_view input, std::wstring& output) {
    for (size_t i = 0; i < input.size(); ++i) {
        const auto c = input[i];
        if ((c < 32 && c != L'\t' && c != L'\n' && c != L'\r') || c == 0xFFFE || c == 0xFFFF)
            return E_INVALIDARG;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 == input.size() || input[i + 1] < 0xDC00 || input[i + 1] > 0xDFFF) return E_INVALIDARG;
            output += c; output += input[++i]; continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) return E_INVALIDARG;
        switch (c) {
        case L'&': output += L"&amp;"; break;
        case L'<': output += L"&lt;"; break;
        case L'>': output += L"&gt;"; break;
        case L'\"': output += L"&quot;"; break;
        case L'\'': output += L"&apos;"; break;
        case L'\t': output += L"&#9;"; break;
        case L'\n': output += L"&#10;"; break;
        case L'\r': output += L"&#13;"; break;
        default: output += c; break;
        }
    }
    return S_OK;
}

HRESULT appendAttribute(std::wstring& xml, const wchar_t* name, std::wstring_view value) {
    xml += L' '; xml += name; xml += L"=\"";
    const auto hr = xmlAttribute(value, xml);
    xml += L'\"';
    return hr;
}

const wchar_t* xmlOperator(CONDITION_OPERATION operation) {
    switch (operation) {
    case COP_IMPLICIT: return L"imp";
    case COP_EQUAL: return L"eq";
    case COP_NOTEQUAL: return L"neq";
    case COP_LESSTHAN: return L"lt";
    case COP_GREATERTHAN: return L"gt";
    case COP_LESSTHANOREQUAL: return L"lte";
    case COP_GREATERTHANOREQUAL: return L"gte";
    case COP_VALUE_STARTSWITH: return L"starts with";
    case COP_VALUE_ENDSWITH: return L"ends with";
    case COP_VALUE_CONTAINS: return L"contains";
    case COP_VALUE_NOTCONTAINS: return L"does not contain";
    case COP_DOSWILDCARDS: return L"matches";
    case COP_WORD_EQUAL: return L"wordmatch";
    // Spellings beyond the examples in the public format are verified by
    // native Windows 10 loader/result parity tests. Reject unverified names.
    default: return nullptr;
    }
}

HRESULT conditionXml(ICondition* condition, std::wstring& xml, unsigned depth, unsigned& count) {
    if (depth > 64 || ++count > 4096) return unsupported;
    CONDITION_TYPE type{};
    auto hr = condition->GetConditionType(&type);
    if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawProperty = nullptr, rawType = nullptr;
        CONDITION_OPERATION operation{};
        Variant value;
        hr = condition->GetComparisonInfo(&rawProperty, &operation, &value.value);
        TaskString property(rawProperty);
        if (FAILED(hr)) return hr;
        hr = condition->GetValueType(&rawType);
        TaskString semanticType(rawType);
        if (FAILED(hr)) return hr;
        const auto* op = xmlOperator(operation);
        // Structured Query's unresolved scalar tokens are strings, including
        // canonical numeric/Boolean/relative-date expressions. Preserve them.
        if (!op || value.value.vt != VT_LPWSTR || !value.value.pwszVal) return unsupported;
        xml += L"<condition type=\"leafCondition\" propertyType=\"wstr\"";
        if (rawProperty && FAILED(hr = appendAttribute(xml, L"property", rawProperty))) return hr;
        if (rawType && FAILED(hr = appendAttribute(xml, L"valuetype", rawType))) return hr;
        if (FAILED(hr = appendAttribute(xml, L"operator", op))) return hr;
        if (FAILED(hr = appendAttribute(xml, L"value", value.value.pwszVal))) return hr;
        xml += L"/>\r\n";
        return S_OK;
    }
    const wchar_t* kind = type == CT_AND_CONDITION ? L"andCondition" :
                          type == CT_OR_CONDITION ? L"orCondition" :
                          type == CT_NOT_CONDITION ? L"notCondition" : nullptr;
    if (!kind) return unsupported;
    xml += L"<condition type=\""; xml += kind; xml += L"\">\r\n";
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        hr = condition->GetSubConditions(IID_PPV_ARGS(&child));
        if (SUCCEEDED(hr)) hr = conditionXml(child.Get(), xml, depth + 1, count);
    } else {
        ComPtr<IEnumUnknown> children;
        hr = condition->GetSubConditions(IID_PPV_ARGS(&children));
        if (FAILED(hr)) return hr;
        unsigned childCount = 0;
        for (;;) {
            ComPtr<IUnknown> unknown;
            hr = children->Next(1, &unknown, nullptr);
            if (hr == S_FALSE) break;
            if (FAILED(hr)) return hr;
            if (!unknown) return E_UNEXPECTED;
            ComPtr<ICondition> child;
            hr = unknown.As(&child);
            if (FAILED(hr)) return hr;
            hr = conditionXml(child.Get(), xml, depth + 1, count);
            if (FAILED(hr)) return hr;
            ++childCount;
        }
        if (childCount < 2) return unsupported;
        hr = S_OK;
    }
    xml += L"</condition>\r\n";
    return hr;
}

HRESULT scopeXml(IShellItem* scope, bool recursive, std::wstring& xml) {
    std::wstring path;
    auto hr = filesystemScope(scope, path);
    xml += L"<scope><include";
    if (SUCCEEDED(hr)) {
        // Scope paths expand environment variables when reopened. A literal
        // percent-delimited directory cannot be persisted faithfully this way.
        if (path.find(L'%') != std::wstring::npos) return unsupported;
        hr = appendAttribute(xml, L"path", path);
    } else {
        if (!recursive) return unsupported;
        ComPtr<IShellItem> computer;
        hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&computer));
        if (FAILED(hr)) return hr;
        int comparison = 1;
        hr = scope->Compare(computer.Get(), SICHINT_CANONICAL, &comparison);
        if (FAILED(hr) || comparison != 0) return unsupported;
        wchar_t identifier[40]{};
        if (!StringFromGUID2(FOLDERID_ComputerFolder, identifier, 40)) return E_UNEXPECTED;
        hr = appendAttribute(xml, L"knownFolder", identifier);
    }
    if (FAILED(hr)) return hr;
    xml += recursive ? L" nonRecursive=\"false\"/></scope>\r\n" :
                       L" nonRecursive=\"true\"/></scope>\r\n";
    return S_OK;
}

HRESULT writeNewFile(const std::filesystem::path& path, const std::wstring& xml) {
    if (xml.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return E_INVALIDARG;
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(),
                                static_cast<int>(xml.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return HRESULT_FROM_WIN32(GetLastError());
    std::string bytes(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(), static_cast<int>(xml.size()),
                              bytes.data(), length, nullptr, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = S_OK;
    size_t written = 0;
    while (written < bytes.size()) {
        DWORD count = 0;
        if (!WriteFile(file, bytes.data() + written, static_cast<DWORD>(bytes.size() - written), &count, nullptr)) {
            hr = HRESULT_FROM_WIN32(GetLastError()); break;
        }
        if (!count) { hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT); break; }
        written += count;
    }
    if (SUCCEEDED(hr) && !FlushFileBuffers(file)) hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
    }
    CloseHandle(file);
    return hr;
}
} // namespace

HRESULT createSearchFolder(const std::wstring& query, IShellItem* scope, IShellItem** result, bool recursive) {
    if (!result) return E_POINTER;
    *result = nullptr;
    std::wstring text;
    auto hr = queryText(query, text);
    if (FAILED(hr)) return hr;

    ComPtr<ICondition> condition;
    hr = parseQuery(text, true, &condition);
    if (FAILED(hr)) return hr;

    ComPtr<IShellItem> searchScope;
    hr = validatedScope(scope, &searchScope);
    if (FAILED(hr)) return hr;
    if (!recursive) {
        std::wstring path;
        hr = filesystemScope(searchScope.Get(), path);
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> restricted;
        hr = restrictToFolder(condition.Get(), path, &restricted);
        if (FAILED(hr)) return hr;
        condition = restricted;
    }

    ComPtr<IShellItemArray> scopes;
    hr = SHCreateShellItemArrayFromShellItem(searchScope.Get(), IID_PPV_ARGS(&scopes));
    if (FAILED(hr)) return hr;
    ComPtr<ISearchFolderItemFactory> factory;
    hr = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    hr = factory->SetScope(scopes.Get());
    if (SUCCEEDED(hr)) hr = factory->SetCondition(condition.Get());
    if (SUCCEEDED(hr)) hr = factory->SetDisplayName(text.c_str());
    if (SUCCEEDED(hr)) hr = factory->SetFolderTypeID(FOLDERTYPEID_GenericSearchResults);
    if (SUCCEEDED(hr)) hr = factory->SetFolderLogicalViewMode(FLVM_DETAILS);
    return FAILED(hr) ? hr : factory->GetShellItem(IID_PPV_ARGS(result));
}

HRESULT saveSearch(const std::wstring& query, IShellItem* scope, bool recursive,
                   const std::filesystem::path& path) {
    try {
        if (path.empty() || path.native().find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        auto extension = path.extension().native();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(towlower(c));
        });
        if (extension != L".search-ms") return E_INVALIDARG;
        std::wstring text;
        auto hr = queryText(query, text);
        if (FAILED(hr)) return hr;
        ComPtr<IShellItem> searchScope;
        hr = validatedScope(scope, &searchScope);
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> condition;
        hr = parseQuery(text, false, &condition);
        if (FAILED(hr)) return hr;
        // Use only documented elements. Leave native view columns and other
        // presentation details to Windows; never write internal attributes.
        std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                           L"<persistedQuery version=\"1.0\">\r\n"
                           L"<viewInfo viewMode=\"details\" iconSize=\"16\"/>\r\n<query>\r\n";
        hr = scopeXml(searchScope.Get(), recursive, xml);
        if (FAILED(hr)) return hr;
        xml += L"<conditions>\r\n";
        unsigned conditionCount = 0;
        hr = conditionXml(condition.Get(), xml, 0, conditionCount);
        if (FAILED(hr)) return hr;
        // Windows 10's loader requires an explicit kind union even when the
        // query does not narrow file kinds. "item" includes every item kind.
        xml += L"</conditions>\r\n<kindList><kind name=\"item\"/></kindList>\r\n"
               L"</query>\r\n</persistedQuery>\r\n";
        return writeNewFile(path, xml);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error& error) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(error.code().value()));
    }
}

} // namespace explorer
