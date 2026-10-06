#include "explorer/search.hpp"
#include "file_security.hpp"
#include "saved_search_internal.hpp"
#include "search_scope_internal.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <structuredquery.h>
#include <propvarutil.h>
#include <propkey.h>
#include <aclapi.h>
#include <winioctl.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string_view>
#include <limits>
#include <vector>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;

constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
constexpr size_t maximumQueryLength = 32768;

struct TaskMemoryFree {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};
using TaskString = std::unique_ptr<wchar_t, TaskMemoryFree>;
struct TaskPidlFree {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(pointer value) const noexcept { CoTaskMemFree(value); }
};
using TaskPidl = std::unique_ptr<ITEMIDLIST, TaskPidlFree>;
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
        return S_OK;
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

HRESULT parseQuery(const std::wstring& text, bool resolve, ICondition** result,
                   IQuerySolution** persistenceResolver = nullptr, IQueryParser** persistenceParser = nullptr) {
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
    if (!resolve) {
        if (persistenceResolver && FAILED(hr = solution.CopyTo(persistenceResolver))) return hr;
        if (persistenceParser && FAILED(hr = parser.CopyTo(persistenceParser))) return hr;
        return parsed.CopyTo(result);
    }

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

HRESULT canonicalScopePath(IShellItem* scope, std::wstring& path) {
    auto hr = filesystemScope(scope, path);
    if (FAILED(hr)) return hr;
    const auto needed = GetLongPathNameW(path.c_str(), nullptr, 0);
    if (!needed) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_INVALID_NAME);
    if (needed > 32768) return unsupported;
    std::wstring canonical(needed, L'\0');
    const auto copied = GetLongPathNameW(path.c_str(), canonical.data(), needed);
    if (!copied || copied >= needed) return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    canonical.resize(copied);
    while (canonical.size() > 3 && (canonical.back() == L'\\' || canonical.back() == L'/')) canonical.pop_back();
    path = std::move(canonical); return S_OK;
}

HRESULT protectiveGuardRequired(const std::vector<SearchScopeRule>& rules, bool& required) {
    if (rules.empty() || rules.size() > 256) return unsupported;
    bool included = false, candidate = false, excluded = false;
    for (const auto& rule : rules) {
        if (!rule.folder) return E_INVALIDARG;
        included = included || !rule.excluded;
        excluded = excluded || rule.excluded;
        candidate = candidate || (rule.excluded && !rule.recursive);
    }
    if (!included) return unsupported;
    if (!excluded) { required = false; return S_OK; }
    std::vector<std::wstring> paths(rules.size());
    for (size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        // A virtual recursive include remains supported with ordinary physical
        // recursive exclusions. Only a required protective guard needs every
        // include to have a complete physical domain.
        if (FAILED(canonicalScopePath(rule.folder.Get(), paths[i]))) paths[i].clear();
    }
    for (size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        if (!rule.excluded) continue;
        ComPtr<IShellItem> parent; std::wstring parentPath;
        if (std::any_of(rules.begin(), rules.end(), [](const auto& include) { return !include.excluded && !include.recursive; })) {
            const auto hr = rule.folder->GetParent(&parent);
            if (FAILED(hr)) return hr;
            if (FAILED(canonicalScopePath(parent.Get(), parentPath))) parentPath.clear();
        }
        for (size_t j = 0; j < rules.size(); ++j) {
            const auto& include = rules[j];
            if (include.excluded) continue;
            int comparison = 1;
            auto hr = rule.folder->Compare(include.folder.Get(), SICHINT_CANONICAL, &comparison);
            if (FAILED(hr)) return hr;
            if (comparison == 0 || (!paths[i].empty() && !paths[j].empty() &&
                CompareStringOrdinal(paths[i].c_str(), -1, paths[j].c_str(), -1, TRUE) == CSTR_EQUAL)) candidate = true;
            if (include.recursive) continue;
            hr = parent->Compare(include.folder.Get(), SICHINT_CANONICAL, &comparison);
            if (FAILED(hr)) return hr;
            if (comparison == 0 || (!parentPath.empty() && !paths[j].empty() &&
                CompareStringOrdinal(parentPath.c_str(), -1, paths[j].c_str(), -1, TRUE) == CSTR_EQUAL)) candidate = true;
        }
    }
    required = candidate; return S_OK;
}

HRESULT normalizedScopes(IShellItemArray* source, IShellItemArray** result) {
    if (!source) return E_INVALIDARG;
    DWORD count = 0;
    auto hr = source->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count || count > 256) return unsupported;
    std::vector<ComPtr<IShellItem>> folders;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(hr = source->GetItemAt(i, &item))) return hr;
        ComPtr<IShellItem> checked;
        if (FAILED(hr = validatedScope(item.Get(), &checked))) return hr;
        ComPtr<IShellItem2> properties;
        PWSTR raw = nullptr;
        const auto typeResult = item.As(&properties);
        if (SUCCEEDED(typeResult)) properties->GetString(PKEY_ItemType, &raw);
        TaskString type(raw);
        if (raw && _wcsicmp(raw, L".library-ms") == 0) {
            ComPtr<IShellLibrary> library;
            hr = CoCreateInstance(CLSID_ShellLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&library));
            if (SUCCEEDED(hr)) hr = library->LoadLibraryFromItem(item.Get(), STGM_READ);
            ComPtr<IShellItemArray> locations;
            if (SUCCEEDED(hr)) hr = library->GetFolders(LFF_FORCEFILESYSTEM, IID_PPV_ARGS(&locations));
            if (FAILED(hr)) return hr;
            DWORD locationCount = 0;
            if (FAILED(hr = locations->GetCount(&locationCount))) return hr;
            if (!locationCount || locationCount > 256 - folders.size()) return unsupported;
            for (DWORD j = 0; j < locationCount; ++j) {
                ComPtr<IShellItem> location;
                if (FAILED(hr = locations->GetItemAt(j, &location))) return hr;
                if (FAILED(hr = validatedScope(location.Get(), &checked))) return hr;
                folders.push_back(std::move(checked));
            }
        } else {
            if (folders.size() == 256) return unsupported;
            folders.push_back(std::move(checked));
        }
    }
    std::vector<TaskPidl> owned;
    std::vector<PCIDLIST_ABSOLUTE> pidls;
    for (const auto& folder : folders) {
        PIDLIST_ABSOLUTE raw = nullptr;
        hr = SHGetIDListFromObject(folder.Get(), &raw);
        TaskPidl pidl(raw);
        if (FAILED(hr)) return hr;
        if (!raw) return E_UNEXPECTED;
        pidls.push_back(raw); owned.push_back(std::move(pidl));
    }
    return SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), pidls.data(), result);
}

HRESULT knownFolderScope(IShellItem* scope, GUID& identifier) {
    PIDLIST_ABSOLUTE raw = nullptr;
    auto hr = SHGetIDListFromObject(scope, &raw);
    TaskPidl pidl(raw);
    if (FAILED(hr)) return hr;
    ComPtr<IKnownFolderManager> manager;
    hr = CoCreateInstance(CLSID_KnownFolderManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;
    ComPtr<IKnownFolder> folder;
    if (FAILED(hr = manager->FindFolderFromIDList(pidl.get(), &folder))) return hr;
    ComPtr<IShellItem> exact;
    if (FAILED(hr = folder->GetShellItem(KF_FLAG_DEFAULT, IID_PPV_ARGS(&exact)))) return hr;
    int comparison = 1;
    hr = scope->Compare(exact.Get(), SICHINT_CANONICAL, &comparison);
    if (FAILED(hr)) return hr;
    return hr == S_OK && comparison == 0 ? folder->GetId(&identifier) : unsupported;
}

HRESULT rulesFromArray(IShellItemArray* source, bool recursive, std::vector<SearchScopeRule>& result) {
    if (!source) return E_INVALIDARG;
    DWORD count = 0;
    auto hr = source->GetCount(&count);
    if (FAILED(hr)) return hr;
    if (!count || count > 256) return unsupported;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(hr = source->GetItemAt(i, &item))) return hr;
        result.push_back({item, recursive, false});
    }
    return S_OK;
}

enum class ScopeDomainValidation { PhysicalLive, NativeDescriptor };

HRESULT normalizeRules(const std::vector<SearchScopeRule>& source, std::vector<SearchScopeRule>& result,
                       ScopeDomainValidation validation) {
    if (source.empty() || source.size() > 256) return unsupported;
    bool included = false;
    for (const auto& rule : source) {
        if (!rule.folder) return E_INVALIDARG;
        ComPtr<IShellItemArray> single, normalized;
        auto hr = SHCreateShellItemArrayFromShellItem(rule.folder.Get(), IID_PPV_ARGS(&single));
        if (SUCCEEDED(hr)) hr = normalizedScopes(single.Get(), &normalized);
        if (FAILED(hr)) return hr;
        DWORD count = 0;
        if (FAILED(hr = normalized->GetCount(&count))) return hr;
        if (count > 256 - result.size()) return unsupported;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(hr = normalized->GetItemAt(i, &item))) return hr;
            result.push_back({item, rule.recursive, rule.excluded});
        }
        included = included || !rule.excluded;
    }
    const bool needsIncludeDomains = std::any_of(result.begin(), result.end(), [](const SearchScopeRule& rule) {
        return !rule.excluded && !rule.recursive;
    });
    for (const auto& rule : result) {
        if (!rule.excluded && !needsIncludeDomains) continue;
        std::wstring path;
        auto hr = filesystemScope(rule.folder.Get(), path);
        if (FAILED(hr) && validation == ScopeDomainValidation::NativeDescriptor && !rule.excluded) {
            // The documented saved scope carries each known folder's depth.
            // The file-free factory instead needs physical include predicates.
            GUID identifier{};
            hr = knownFolderScope(rule.folder.Get(), identifier);
        }
        if (FAILED(hr)) return hr;
    }
    if (!included) return unsupported;
    bool guarded = false;
    auto hr = protectiveGuardRequired(result, guarded);
    if (SUCCEEDED(hr) && guarded) for (const auto& rule : result) {
        std::wstring path;
        if (FAILED(hr = canonicalScopePath(rule.folder.Get(), path))) return hr;
    }
    return hr;
}

HRESULT includedArray(const std::vector<SearchScopeRule>& rules, IShellItemArray** result) {
    std::vector<TaskPidl> owned;
    std::vector<PCIDLIST_ABSOLUTE> pidls;
    for (const auto& rule : rules) {
        if (rule.excluded) continue;
        PIDLIST_ABSOLUTE raw = nullptr;
        auto hr = SHGetIDListFromObject(rule.folder.Get(), &raw);
        TaskPidl pidl(raw);
        if (FAILED(hr)) return hr;
        if (!raw) return E_UNEXPECTED;
        pidls.push_back(raw); owned.push_back(std::move(pidl));
    }
    if (pidls.empty() || pidls.size() > 256) return unsupported;
    return SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), pidls.data(), result);
}

HRESULT scopeDomain(IConditionFactory2* factory, const SearchScopeRule& rule, ICondition** result, bool canonicalGuard = false) {
    std::wstring path;
    auto hr = canonicalGuard ? canonicalScopePath(rule.folder.Get(), path) : filesystemScope(rule.folder.Get(), path);
    if (FAILED(hr)) return hr;
    // Match the provider's canonical long filesystem paths, including when
    // the supplied scope used an existing 8.3 alias.
    if (!canonicalGuard) {
        const auto needed = GetLongPathNameW(path.c_str(), nullptr, 0);
        if (needed && needed <= 32768) {
            std::wstring canonical(needed, L'\0');
            const auto copied = GetLongPathNameW(path.c_str(), canonical.data(), needed);
            if (copied && copied < needed) { canonical.resize(copied); path = std::move(canonical); }
        }
    }
    std::vector<ComPtr<ICondition>> owned;
    std::vector<ICondition*> children;
    auto leaf = [&](const wchar_t* property, CONDITION_OPERATION operation, const std::wstring& text) {
        Variant value;
        auto made = InitPropVariantFromString(text.c_str(), &value.value);
        ComPtr<ICondition> condition;
        if (SUCCEEDED(made)) made = factory->MakeLeaf(property, operation,
            canonicalGuard ? L"System.StructuredQueryType.String" : nullptr, &value.value,
            nullptr, nullptr, nullptr, FALSE, &condition);
        if (SUCCEEDED(made)) { children.push_back(condition.Get()); owned.push_back(std::move(condition)); }
        return made;
    };
    if (FAILED(hr = leaf(L"System.ItemFolderPathDisplay", COP_EQUAL, path))) return hr;
    if (rule.recursive) {
        auto prefix = path;
        if (prefix.back() != L'\\' && prefix.back() != L'/') prefix += L'\\';
        if (FAILED(hr = leaf(L"System.ItemFolderPathDisplay", COP_VALUE_STARTSWITH, prefix))) return hr;
    }
    if (rule.excluded && FAILED(hr = leaf(L"System.ItemPathDisplay", COP_EQUAL, path))) return hr;
    if (children.size() == 1) return owned.front().CopyTo(result);
    return factory->CreateCompoundFromArray(CT_OR_CONDITION, children.data(), static_cast<ULONG>(children.size()),
        CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}

HRESULT scopeGuard(const std::vector<SearchScopeRule>& rules, ICondition** result) {
    bool ignored = false;
    auto hr = protectiveGuardRequired(rules, ignored);
    if (FAILED(hr)) return hr;
    ComPtr<IConditionFactory2> factory;
    hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    std::array<std::vector<ComPtr<ICondition>>, 2> domains;
    for (const auto& rule : rules) {
        ComPtr<ICondition> domain;
        if (FAILED(hr = scopeDomain(factory.Get(), rule, &domain, true))) return hr;
        domains[rule.excluded ? 1 : 0].push_back(std::move(domain));
    }
    std::vector<ComPtr<ICondition>> clauses;
    for (unsigned exclude = 0; exclude < domains.size(); ++exclude) {
        if (domains[exclude].empty()) continue;
        ComPtr<ICondition> combined;
        if (domains[exclude].size() == 1) combined = domains[exclude].front();
        else {
            std::vector<ICondition*> raw;
            for (const auto& domain : domains[exclude]) raw.push_back(domain.Get());
            if (FAILED(hr = factory->CreateCompoundFromArray(CT_OR_CONDITION, raw.data(), static_cast<ULONG>(raw.size()),
                CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&combined)))) return hr;
        }
        if (exclude) {
            ComPtr<ICondition> inverse;
            if (FAILED(hr = factory->MakeNot(combined.Get(), FALSE, &inverse))) return hr;
            combined = std::move(inverse);
        }
        clauses.push_back(std::move(combined));
    }
    if (clauses.size() == 1) return clauses.front().CopyTo(result);
    std::vector<ICondition*> raw;
    for (const auto& clause : clauses) raw.push_back(clause.Get());
    return factory->CreateCompoundFromArray(CT_AND_CONDITION, raw.data(), static_cast<ULONG>(raw.size()),
        CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}

HRESULT guardChildren(ICondition* condition, std::vector<ComPtr<ICondition>>& result) {
    CONDITION_TYPE type{};
    auto hr = condition->GetConditionType(&type); if (FAILED(hr)) return hr;
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        hr = condition->GetSubConditions(IID_PPV_ARGS(&child));
        if (SUCCEEDED(hr) && child) result.push_back(std::move(child));
        return FAILED(hr) ? hr : result.empty() ? unsupported : S_OK;
    }
    if (type != CT_AND_CONDITION && type != CT_OR_CONDITION) return unsupported;
    ComPtr<IEnumUnknown> enumeration;
    hr = condition->GetSubConditions(IID_PPV_ARGS(&enumeration)); if (FAILED(hr)) return hr;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) return hr;
        if (!unknown || result.size() == 4096) return unsupported;
        ComPtr<ICondition> child;
        if (FAILED(hr = unknown.As(&child))) return hr;
        result.push_back(std::move(child));
    }
    return result.empty() ? unsupported : S_OK;
}

HRESULT guardSignature(ICondition* condition, std::wstring& result, unsigned& nodes, unsigned depth = 0) {
    constexpr size_t maximumSignature = 4 * 1024 * 1024;
    if (!condition || depth > 64 || ++nodes > 4096) return unsupported;
    CONDITION_TYPE type{};
    auto hr = condition->GetConditionType(&type); if (FAILED(hr)) return hr;
    const auto field = [&](std::wstring_view value) { result += std::to_wstring(value.size()) + L":"; result += value; };
    if (type == CT_LEAF_CONDITION) {
        PWSTR rawProperty = nullptr, rawSemantic = nullptr;
        CONDITION_OPERATION operation{}; Variant value;
        hr = condition->GetComparisonInfo(&rawProperty, &operation, &value.value); TaskString property(rawProperty);
        if (FAILED(hr)) return hr;
        hr = condition->GetValueType(&rawSemantic); TaskString semantic(rawSemantic);
        if (FAILED(hr)) return hr;
        if ((rawProperty && wcslen(rawProperty) > 32768) || (rawSemantic && wcslen(rawSemantic) > 32768)) return unsupported;
        std::wstring canonical = rawProperty ? rawProperty : L"";
        std::transform(canonical.begin(), canonical.end(), canonical.begin(), [](wchar_t code) { return static_cast<wchar_t>(towlower(code)); });
        result = L"3{"; result += rawProperty ? L"1:" : L"0:"; field(canonical);
        result += rawSemantic ? L"1:" : L"0:"; field(rawSemantic ? rawSemantic : L"");
        result += std::to_wstring(operation) + L":" + std::to_wstring(value.value.vt) + L":";
        SERIALIZEDPROPERTYVALUE* bytes = nullptr; ULONG count = 0;
        hr = StgSerializePropVariant(&value.value, &bytes, &count);
        const std::unique_ptr<SERIALIZEDPROPERTYVALUE, decltype(&CoTaskMemFree)> storage(bytes, &CoTaskMemFree);
        if (FAILED(hr)) return hr;
        if ((!bytes && count) || count > maximumSignature / 2) return unsupported;
        constexpr wchar_t hex[] = L"0123456789abcdef";
        const auto data = reinterpret_cast<const BYTE*>(bytes);
        for (ULONG i = 0; i < count; ++i) { result += hex[data[i] >> 4]; result += hex[data[i] & 15]; }
        result += L"}"; return result.size() > maximumSignature ? unsupported : S_OK;
    }
    std::vector<std::wstring> signatures;
    std::function<HRESULT(ICondition*, unsigned)> append = [&](ICondition* child, unsigned level) -> HRESULT {
        if (!child || level > 64 || ++nodes > 4096) return unsupported;
        CONDITION_TYPE childType{}; auto status = child->GetConditionType(&childType); if (FAILED(status)) return status;
        if (type != CT_NOT_CONDITION && childType == type) {
            std::vector<ComPtr<ICondition>> nested;
            if (FAILED(status = guardChildren(child, nested))) return status;
            for (const auto& node : nested) if (FAILED(status = append(node.Get(), level + 1))) return status;
            return S_OK;
        }
        std::wstring signature;
        status = guardSignature(child, signature, nodes, level);
        if (SUCCEEDED(status)) signatures.push_back(std::move(signature));
        return status;
    };
    std::vector<ComPtr<ICondition>> children;
    if (FAILED(hr = guardChildren(condition, children))) return hr;
    for (const auto& child : children) if (FAILED(hr = append(child.Get(), depth + 1))) return hr;
    if (type != CT_NOT_CONDITION) {
        std::sort(signatures.begin(), signatures.end());
        signatures.erase(std::unique(signatures.begin(), signatures.end()), signatures.end());
        if (signatures.size() == 1) { result = std::move(signatures.front()); return S_OK; }
    }
    result = std::to_wstring(type) + L"{";
    for (const auto& signature : signatures) { field(signature); if (result.size() > maximumSignature) return unsupported; }
    result += L"}"; return S_OK;
}

HRESULT restrictToRules(ICondition* query, const std::vector<SearchScopeRule>& rules, ICondition** result) {
    const bool mixed = std::any_of(rules.begin(), rules.end(), [](const SearchScopeRule& rule) { return !rule.excluded && !rule.recursive; });
    const bool excluded = std::any_of(rules.begin(), rules.end(), [](const SearchScopeRule& rule) { return rule.excluded; });
    if (!mixed && !excluded) return query->QueryInterface(IID_PPV_ARGS(result));
    bool guarded = false;
    auto hr = protectiveGuardRequired(rules, guarded);
    if (FAILED(hr)) return hr;
    ComPtr<IConditionFactory2> factory;
    hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    if (guarded) {
        ComPtr<ICondition> guard;
        if (FAILED(hr = scopeGuard(rules, &guard))) return hr;
        ICondition* clauses[]{query, guard.Get()};
        return factory->CreateCompoundFromArray(CT_AND_CONDITION, clauses, 2, CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
    }
    std::vector<ComPtr<ICondition>> includedDomains, excludedDomains, owned;
    std::vector<ICondition*> clauses{query};
    for (const auto& rule : rules) {
        if (!rule.excluded && !mixed) continue;
        ComPtr<ICondition> domain;
        if (FAILED(hr = scopeDomain(factory.Get(), rule, &domain))) return hr;
        (rule.excluded ? excludedDomains : includedDomains).push_back(std::move(domain));
    }
    for (const auto isExclude : {false, true}) {
        auto& domains = isExclude ? excludedDomains : includedDomains;
        if (domains.empty()) continue;
        ComPtr<ICondition> combined;
        if (domains.size() == 1) combined = domains.front();
        else {
            std::vector<ICondition*> raw;
            for (const auto& domain : domains) raw.push_back(domain.Get());
            if (FAILED(hr = factory->CreateCompoundFromArray(CT_OR_CONDITION, raw.data(), static_cast<ULONG>(raw.size()),
                CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&combined)))) return hr;
        }
        if (isExclude) {
            ComPtr<ICondition> inverse;
            if (FAILED(hr = factory->MakeNot(combined.Get(), FALSE, &inverse))) return hr;
            combined = std::move(inverse);
        }
        clauses.push_back(combined.Get()); owned.push_back(std::move(combined));
    }
    return factory->CreateCompoundFromArray(CT_AND_CONDITION, clauses.data(), static_cast<ULONG>(clauses.size()),
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
    case COP_WORD_EQUAL: return L"word eq";
    case COP_WORD_STARTSWITH: return L"wordmatch";
    // Spellings beyond the examples in the public format are verified by
    // native Windows 10 loader/result parity tests. Reject unverified names.
    default: return nullptr;
    }
}

HRESULT conditionXml(ICondition* condition, std::wstring& xml, unsigned depth, unsigned& count,
                     IQuerySolution* resolver = nullptr, IQueryParser* parser = nullptr, bool genericResolved = false) {
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
        const bool parserStringToken = rawType && wcscmp(rawType, L"System.StructuredQueryType.Blurb") == 0;
        if (!rawProperty || (parserStringToken && !genericResolved)) {
            if (!resolver || genericResolved) return unsupported;
            // Generic and named Blurb tokens require their original parser
            // context. FileExtension, for example, resolves to a String leaf;
            // emitting its unresolved Blurb value makes the loader return no
            // results. Date siblings retain their unresolved representation.
            ComPtr<ICondition> resolved;
            hr = resolver->Resolve(condition,
                static_cast<STRUCTURED_QUERY_RESOLVE_OPTION>(SQRO_DONT_SPLIT_WORDS | SQRO_DONT_RESOLVE_DATETIME),
                nullptr, &resolved);
            if (FAILED(hr)) return hr;
            return resolved ? conditionXml(resolved.Get(), xml, depth + 1, count, nullptr, parser, true) : E_UNEXPECTED;
        }
        if (value.value.vt == (VT_VECTOR | VT_LPWSTR)) {
            // A native AQS date range is an inclusive pair of unresolved date
            // tokens. The public XML format stores scalar leaves, so retain
            // both tokens in >= / <= leaves instead of freezing relative dates
            // or discarding the range's end. Other vector shapes stay rejected.
            if (!resolver || (operation != COP_EQUAL && operation != COP_IMPLICIT) || !rawType ||
                wcscmp(rawType, L"System.StructuredQueryType.DateTime") != 0 ||
                value.value.calpwstr.cElems != 2 || !value.value.calpwstr.pElems)
                return unsupported;
            std::array<ComPtr<ICondition>, 2> bounds;
            for (unsigned index = 0; index < bounds.size(); ++index) {
                const auto token = value.value.calpwstr.pElems[index];
                if (!token || !*token || wcslen(token) > maximumQueryLength) return unsupported;
                Variant scalar;
                if (FAILED(hr = InitPropVariantFromString(token, &scalar.value))) return hr;
                hr = resolver->MakeLeaf(rawProperty,
                    index == 0 ? COP_GREATERTHANOREQUAL : COP_LESSTHANOREQUAL, rawType,
                    &scalar.value, nullptr, nullptr, nullptr, FALSE, &bounds[index]);
                if (FAILED(hr)) return hr;
            }
            xml += L"<condition type=\"andCondition\">\r\n";
            for (const auto& bound : bounds)
                if (FAILED(hr = conditionXml(bound.Get(), xml, depth + 1, count, resolver, parser))) return hr;
            xml += L"</condition>\r\n";
            return S_OK;
        }
        if (rawType && !saved_search_internal::supportedValueType(rawType)) return unsupported;
        const auto* op = xmlOperator(operation);
        const wchar_t* text = nullptr;
        if (value.value.vt == VT_LPWSTR) text = value.value.pwszVal;
        else if (genericResolved) {
            if (!parser) return unsupported;
            switch (value.value.vt) {
            case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4:
            case VT_I8: case VT_UI8: case VT_R4: case VT_R8: case VT_BOOL: {
                // Let the public parser produce its own unresolved scalar
                // token. A numeric leaf's plain "123" is not equivalent to
                // the native encoded "=123" token used by the XML loader.
                PWSTR raw = nullptr;
                hr = parser->RestateToString(condition, FALSE, &raw);
                TaskString restated(raw);
                if (FAILED(hr)) return hr;
                if (!raw || wcslen(raw) > maximumQueryLength) return unsupported;
                ComPtr<IQuerySolution> solution;
                hr = parser->Parse(raw, nullptr, &solution);
                if (FAILED(hr)) return hr;
                ComPtr<ICondition> unresolved, checked;
                hr = solution->GetQuery(&unresolved, nullptr);
                if (SUCCEEDED(hr)) hr = solution->Resolve(unresolved.Get(),
                    static_cast<STRUCTURED_QUERY_RESOLVE_OPTION>(SQRO_DONT_SPLIT_WORDS | SQRO_DONT_RESOLVE_DATETIME),
                    nullptr, &checked);
                if (FAILED(hr)) return hr;
                if (!unresolved || !checked) return E_UNEXPECTED;
                CONDITION_TYPE checkedType{};
                if (FAILED(hr = checked->GetConditionType(&checkedType))) return hr;
                if (checkedType != CT_LEAF_CONDITION) return unsupported;
                PWSTR checkedRawProperty = nullptr;
                CONDITION_OPERATION checkedOperation{};
                Variant checkedValue;
                hr = checked->GetComparisonInfo(&checkedRawProperty, &checkedOperation, &checkedValue.value);
                TaskString checkedProperty(checkedRawProperty);
                if (FAILED(hr)) return hr;
                if (!checkedRawProperty || _wcsicmp(rawProperty, checkedRawProperty) != 0 ||
                    operation != checkedOperation || value.value.vt != checkedValue.value.vt ||
                    PropVariantCompareEx(value.value, checkedValue.value, PVCU_DEFAULT, PVCF_DEFAULT) != 0)
                    return unsupported;
                return conditionXml(unresolved.Get(), xml, depth + 1, count, nullptr, nullptr, false);
            }
            default: return unsupported;
            }
        }
        // Structured Query's unresolved scalar tokens are strings, including
        // canonical numeric/Boolean/relative-date expressions. Preserve them.
        if (!op || !text) return unsupported;
        xml += L"<condition type=\"leafCondition\" propertyType=\"wstr\"";
        if (FAILED(hr = appendAttribute(xml, L"property", rawProperty))) return hr;
        if (rawType && FAILED(hr = appendAttribute(xml, L"valuetype", rawType))) return hr;
        if (FAILED(hr = appendAttribute(xml, L"operator", op))) return hr;
        if (FAILED(hr = appendAttribute(xml, L"value", text))) return hr;
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
        if (SUCCEEDED(hr)) hr = conditionXml(child.Get(), xml, depth + 1, count, resolver, parser, genericResolved);
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
            hr = conditionXml(child.Get(), xml, depth + 1, count, resolver, parser, genericResolved);
            if (FAILED(hr)) return hr;
            ++childCount;
        }
        if (childCount < 2) return unsupported;
        hr = S_OK;
    }
    xml += L"</condition>\r\n";
    return hr;
}

HRESULT scopeIncludeXml(IShellItem* scope, bool recursive, bool excluded, std::wstring& xml) {
    std::wstring path;
    auto hr = filesystemScope(scope, path);
    xml += excluded ? L"<exclude" : L"<include";
    if (SUCCEEDED(hr)) {
        // Scope paths expand environment variables when reopened. A literal
        // percent-delimited directory cannot be persisted faithfully this way.
        const auto percent = path.find(L'%');
        if (percent != std::wstring::npos && path.find(L'%', percent + 1) != std::wstring::npos) return unsupported;
        hr = appendAttribute(xml, L"path", path);
    } else {
        GUID folder{};
        if (FAILED(knownFolderScope(scope, folder))) return unsupported;
        wchar_t identifier[40]{};
        if (!StringFromGUID2(folder, identifier, 40)) return E_UNEXPECTED;
        hr = appendAttribute(xml, L"knownFolder", identifier);
    }
    if (FAILED(hr)) return hr;
    xml += recursive ? L" nonRecursive=\"false\"/>\r\n" :
                       L" nonRecursive=\"true\"/>\r\n";
    return S_OK;
}

HRESULT scopeXml(const std::vector<SearchScopeRule>& scopes, std::wstring& xml) {
    xml += L"<scope>";
    for (const auto excluded : {false, true}) for (const auto& rule : scopes)
        if (rule.excluded == excluded) {
            const auto hr = scopeIncludeXml(rule.folder.Get(), rule.recursive, rule.excluded, xml);
            if (FAILED(hr)) return hr;
        }
    xml += L"</scope>\r\n";
    return S_OK;
}

struct SaveFile {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool retain = false;
    ~SaveFile() {
        if (value == INVALID_HANDLE_VALUE) return;
        if (!retain) {
            // Dispose by the create-new handle, never by a potentially reused
            // temporary filename. The requested DELETE access also allows
            // cleanup when an inherited ACL forbids later path-based deletion.
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(value, FileDispositionInfo, &remove, sizeof(remove));
        }
        CloseHandle(value);
    }
};
HRESULT xmlBytes(const std::wstring& xml, std::string& bytes) {
    if (xml.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return E_INVALIDARG;
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(),
                                static_cast<int>(xml.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return HRESULT_FROM_WIN32(GetLastError());
    bytes.assign(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(), static_cast<int>(xml.size()),
                              bytes.data(), length, nullptr, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}
HRESULT writeContents(HANDLE file, const std::string& bytes) {
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
    return hr;
}

HRESULT filePropertiesXml(const SearchFileProperties& properties, std::wstring& xml) {
    xml += L"<properties>\r\n";
    const auto field = [&](const wchar_t* name, const std::optional<std::wstring>& value) -> HRESULT {
        if (!value) return S_OK;
        if (value->size() > maximumQueryLength) return E_INVALIDARG;
        xml += L"<" + std::wstring(name) + L">";
        // Escaping whitespace also avoids XML end-of-line normalization
        // changing an imported literal value on the following read.
        const auto hr = xmlAttribute(*value, xml);
        if (FAILED(hr)) return hr;
        xml += L"</" + std::wstring(name) + L">\r\n";
        return S_OK;
    };
    auto hr = field(L"author", properties.author);
    if (SUCCEEDED(hr)) hr = field(L"kind", properties.kind);
    if (SUCCEEDED(hr)) hr = field(L"description", properties.description);
    if (SUCCEEDED(hr)) hr = field(L"tags", properties.tags);
    if (FAILED(hr)) return hr;
    xml += L"</properties>\r\n";
    return S_OK;
}
HRESULT writeNewFile(const std::filesystem::path& path, const std::string& bytes,
                     SearchCreatedFileProof* createdFileProof = nullptr) {
    SaveFile file;
    file.value = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE | (createdFileProof ? FILE_READ_ATTRIBUTES : 0), 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    auto hr = writeContents(file.value, bytes);
    if (SUCCEEDED(hr) && createdFileProof) {
        FILE_ID_INFO identity{};
        if (!GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity)))
            hr = HRESULT_FROM_WIN32(GetLastError());
        else {
            createdFileProof->identity = identity;
            createdFileProof->bytes = bytes;
            createdFileProof->captured = true;
        }
    }
    file.retain = SUCCEEDED(hr);
    return hr;
}
HRESULT writeConfirmedFile(const std::filesystem::path& path, const std::string& bytes, bool& replaced) {
    replaced = false;
    // Keep a reference to the confirmed file while preparing the new data.
    // Request DELETE access before creating a temporary: this observes readers
    // that deny replacement, even with the POSIX rename publication below.
    // Existing writers or new writable handles prevent this operation.
    // Native rename still needs SHARE_DELETE; validate the pathname identity
    // again immediately before publication instead of trusting an old path.
    SaveFile original;
    original.retain = true;
    original.value = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (original.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        // Do not replace a file created after the Save dialog selected a new
        // name. That race remains a create-new collision for the caller.
        return error == ERROR_FILE_NOT_FOUND ? writeNewFile(path, bytes) : HRESULT_FROM_WIN32(error);
    }
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(original.value, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(original.value, FileIdInfo, &identity, sizeof(identity)))
        return HRESULT_FROM_WIN32(GetLastError());
    if (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY))
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    // Preserve confidentiality rather than silently turning an EFS file into
    // an unencrypted temporary file on a nonencrypted parent directory.
    if (basic.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED) return unsupported;
    USHORT compression = COMPRESSION_FORMAT_NONE;
    DWORD returned = 0;
    if ((basic.FileAttributes & FILE_ATTRIBUTE_COMPRESSED) &&
        !DeviceIoControl(original.value, FSCTL_GET_COMPRESSION, nullptr, 0, &compression, sizeof(compression), &returned, nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
    file_security::Descriptor security;
    auto hr = file_security::read(original.value, security);
    if (FAILED(hr)) return hr;
    auto attributes = file_security::attributes(security);

    const auto absolute = std::filesystem::absolute(path);
    const auto parent = absolute.parent_path();
    SaveFile temporary;
    std::filesystem::path temporaryPath;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        GUID id{};
        hr = CoCreateGuid(&id);
        if (FAILED(hr)) return hr;
        wchar_t text[40]{};
        if (!StringFromGUID2(id, text, 40)) return E_FAIL;
        temporaryPath = parent / (std::wstring(L".WindowsExplorer-search-save-") + text + L".tmp");
        temporary.value = CreateFileW(temporaryPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.value != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(error);
    }
    if (temporary.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    hr = file_security::verifyCreated(temporary.value, security);
    if (FAILED(hr)) return hr;
    if (compression != COMPRESSION_FORMAT_NONE &&
        !DeviceIoControl(temporary.value, FSCTL_SET_COMPRESSION, &compression, sizeof(compression), nullptr, 0, &returned, nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
    hr = writeContents(temporary.value, bytes);
    if (FAILED(hr)) return hr;
    FILE_BASIC_INFO retained{};
    retained.CreationTime = basic.CreationTime;
    retained.FileAttributes = basic.FileAttributes &
        (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
    if (!retained.FileAttributes) retained.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileInformationByHandle(temporary.value, FileBasicInfo, &retained, sizeof(retained)))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!FlushFileBuffers(temporary.value)) return HRESULT_FROM_WIN32(GetLastError());
    {
        SaveFile current;
        current.retain = true;
        current.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (current.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        FILE_ID_INFO now{};
        if (!GetFileInformationByHandleEx(current.value, FileIdInfo, &now, sizeof(now)))
            return HRESULT_FROM_WIN32(GetLastError());
        if (now.VolumeSerialNumber != identity.VolumeSerialNumber ||
            std::memcmp(now.FileId.Identifier, identity.FileId.Identifier, sizeof(now.FileId.Identifier)) != 0)
            return HRESULT_FROM_WIN32(ERROR_RETRY);
        file_security::Descriptor currentSecurity;
        hr = file_security::read(original.value, currentSecurity);
        if (FAILED(hr)) return hr;
        if (!file_security::equal(security, currentSecurity)) return HRESULT_FROM_WIN32(ERROR_RETRY);
        hr = file_security::read(temporary.value, currentSecurity);
        if (FAILED(hr)) return hr;
        if (!file_security::equal(security, currentSecurity)) return HRESULT_FROM_WIN32(ERROR_RETRY);
        // Publish by the owned handle in one native same-directory rename.
        // This also preserves ordinary hidden/system targets, which the path-
        // based MoveFileEx replacement can reject. ReplaceFile without a
        // backup has documented partial failures that can remove the original.
        const auto name = absolute.native();
        if (name.size() > ((std::numeric_limits<DWORD>::max)() - offsetof(FILE_RENAME_INFO, FileName) - sizeof(wchar_t)) /
                          sizeof(wchar_t)) return E_INVALIDARG;
        const auto renameBytes = name.size() * sizeof(wchar_t);
        // Keep an explicit terminator for Win32 path translation; the native
        // byte count excludes it.
        std::vector<BYTE> storage(offsetof(FILE_RENAME_INFO, FileName) + renameBytes + sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        // Windows 10 permits the confirmed target's read handles to continue
        // referring to the old file while the pathname names the new file.
        rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(renameBytes);
        std::memcpy(rename->FileName, name.data(), renameBytes);
        if (!SetFileInformationByHandle(temporary.value, FileRenameInfoEx, rename, static_cast<DWORD>(storage.size())))
            return HRESULT_FROM_WIN32(GetLastError());
    }
    temporary.retain = true;
    replaced = true;
    return S_OK;
}
} // namespace

HRESULT validateSearchViewPresentation(const SearchViewPresentation& presentation) {
    if (presentation.mode && *presentation.mode != SearchViewMode::Details &&
        *presentation.mode != SearchViewMode::Icons && *presentation.mode != SearchViewMode::Tiles &&
        *presentation.mode != SearchViewMode::SmallIcons && *presentation.mode != SearchViewMode::List &&
        *presentation.mode != SearchViewMode::Content) return E_INVALIDARG;
    if (presentation.iconSize && (*presentation.iconSize < 16 || *presentation.iconSize > 256)) return E_INVALIDARG;
    const auto property = [](const std::wstring& name) {
        if (name.empty() || name.size() > 512 || name.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        PROPERTYKEY key{};
        const auto hr = PSGetPropertyKeyFromName(name.c_str(), &key);
        return FAILED(hr) ? unsupported : S_OK;
    };
    const auto order = [&](const SearchViewOrder& value) {
        if (value.direction != SORT_ASCENDING && value.direction != SORT_DESCENDING) return E_INVALIDARG;
        return property(value.property);
    };
    if (presentation.visibleColumns) {
        if (presentation.visibleColumns->empty() || presentation.visibleColumns->size() > 128) return unsupported;
        std::vector<PROPERTYKEY> seen;
        for (const auto& name : *presentation.visibleColumns) {
            auto hr = property(name); if (FAILED(hr)) return hr;
            PROPERTYKEY key{}; if (FAILED(hr = PSGetPropertyKeyFromName(name.c_str(), &key))) return hr;
            if (std::any_of(seen.begin(), seen.end(), [&](const PROPERTYKEY& item) { return IsEqualPropertyKey(item, key); })) return E_INVALIDARG;
            seen.push_back(key);
        }
    }
    if (presentation.groupBy) { const auto hr = order(*presentation.groupBy); if (FAILED(hr)) return hr; }
    if (presentation.sort) {
        if (presentation.sort->size() > 4) return unsupported;
        std::vector<PROPERTYKEY> seen;
        for (const auto& value : *presentation.sort) {
            auto hr = order(value); if (FAILED(hr)) return hr;
            PROPERTYKEY key{}; if (FAILED(hr = PSGetPropertyKeyFromName(value.property.c_str(), &key))) return hr;
            if (std::any_of(seen.begin(), seen.end(), [&](const PROPERTYKEY& item) { return IsEqualPropertyKey(item, key); })) return E_INVALIDARG;
            seen.push_back(key);
        }
    }
    return S_OK;
}

HRESULT captureSearchViewPresentation(IFolderView2* view, SearchViewPresentation* result) {
    if (!view || !result) return E_POINTER;
    try {
        SearchViewPresentation candidate;
        FOLDERVIEWMODE mode{}; int size = 0;
        auto hr = view->GetViewModeAndIconSize(&mode, &size); if (FAILED(hr)) return hr;
        if (mode == FVM_DETAILS) candidate.mode = SearchViewMode::Details;
        else if (mode == FVM_ICON) candidate.mode = SearchViewMode::Icons;
        else if (mode == FVM_TILE) candidate.mode = SearchViewMode::Tiles;
        else if (mode == FVM_SMALLICON) candidate.mode = SearchViewMode::SmallIcons;
        else if (mode == FVM_LIST) candidate.mode = SearchViewMode::List;
        else if (mode == FVM_CONTENT) candidate.mode = SearchViewMode::Content;
        else return unsupported;
        candidate.iconSize = size;
        const auto name = [](REFPROPERTYKEY key, std::wstring& text) {
            PWSTR raw = nullptr;
            const auto status = PSGetNameFromPropertyKey(key, &raw);
            TaskString owned(raw);
            if (FAILED(status)) return status;
            if (!raw || !*raw) return unsupported;
            text = raw; return S_OK;
        };
        ComPtr<IColumnManager> columns;
        if (FAILED(hr = view->QueryInterface(IID_PPV_ARGS(&columns)))) return hr;
        UINT count = 0; if (FAILED(hr = columns->GetColumnCount(CM_ENUM_VISIBLE, &count))) return hr;
        if (!count || count > 128) return unsupported;
        std::vector<PROPERTYKEY> keys(count);
        if (FAILED(hr = columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), count))) return hr;
        candidate.visibleColumns.emplace();
        for (const auto& key : keys) {
            std::wstring canonical; if (FAILED(hr = name(key, canonical))) return hr;
            candidate.visibleColumns->push_back(std::move(canonical));
        }
        PROPERTYKEY group{}; BOOL ascending = FALSE;
        if (FAILED(hr = view->GetGroupBy(&group, &ascending))) return hr;
        candidate.groupBy.emplace();
        if (FAILED(hr = name(group, candidate.groupBy->property))) return hr;
        candidate.groupBy->direction = ascending ? SORT_ASCENDING : SORT_DESCENDING;
        int sortCount = 0; if (FAILED(hr = view->GetSortColumnCount(&sortCount))) return hr;
        if (sortCount < 0 || sortCount > 4) return unsupported;
        std::vector<SORTCOLUMN> sorts(static_cast<size_t>(sortCount));
        if (sortCount && FAILED(hr = view->GetSortColumns(sorts.data(), sortCount))) return hr;
        candidate.sort.emplace();
        for (const auto& value : sorts) {
            SearchViewOrder entry; entry.direction = value.direction;
            if (FAILED(hr = name(value.propkey, entry.property))) return hr;
            candidate.sort->push_back(std::move(entry));
        }
        if (FAILED(hr = validateSearchViewPresentation(candidate))) return hr;
        *result = std::move(candidate); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT applySearchViewPresentation(IFolderView2* view, const SearchViewPresentation& presentation) {
    if (!view) return E_POINTER;
    try {
        auto hr = validateSearchViewPresentation(presentation); if (FAILED(hr)) return hr;
        ComPtr<IColumnManager> columns;
        std::vector<PROPERTYKEY> keys;
        if (presentation.visibleColumns) {
            if (FAILED(hr = view->QueryInterface(IID_PPV_ARGS(&columns)))) return hr;
            for (const auto& name : *presentation.visibleColumns) {
                PROPERTYKEY key{}; if (FAILED(hr = PSGetPropertyKeyFromName(name.c_str(), &key))) return hr;
                keys.push_back(key);
            }
        }
        PROPERTYKEY group{};
        if (presentation.groupBy && FAILED(hr = PSGetPropertyKeyFromName(presentation.groupBy->property.c_str(), &group))) return hr;
        std::vector<SORTCOLUMN> sorts;
        if (presentation.sort) for (const auto& order : *presentation.sort) {
            SORTCOLUMN value{}; value.direction = order.direction;
            if (FAILED(hr = PSGetPropertyKeyFromName(order.property.c_str(), &value.propkey))) return hr;
            sorts.push_back(value);
        }
        if (columns && FAILED(hr = columns->SetColumns(keys.data(), static_cast<UINT>(keys.size())))) return hr;
        if (presentation.groupBy && FAILED(hr = view->SetGroupBy(group, presentation.groupBy->direction == SORT_ASCENDING))) return hr;
        if (presentation.sort && !sorts.empty() && FAILED(hr = view->SetSortColumns(sorts.data(), static_cast<int>(sorts.size())))) return hr;
        if (presentation.mode || presentation.iconSize) {
            FOLDERVIEWMODE mode{}; int size = 0;
            if (FAILED(hr = view->GetViewModeAndIconSize(&mode, &size))) return hr;
            if (presentation.mode) {
                switch (*presentation.mode) {
                case SearchViewMode::Details: mode = FVM_DETAILS; break;
                case SearchViewMode::Icons: mode = FVM_ICON; break;
                case SearchViewMode::Tiles: mode = FVM_TILE; break;
                case SearchViewMode::SmallIcons: mode = FVM_SMALLICON; break;
                case SearchViewMode::List: mode = FVM_LIST; break;
                case SearchViewMode::Content: mode = FVM_CONTENT; break;
                }
            }
            if (presentation.iconSize) size = *presentation.iconSize;
            if (FAILED(hr = view->SetViewModeAndIconSize(mode, size))) return hr;
        }
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT nativeSearchViewPresentation(const SearchViewPresentation& actual, SearchViewPresentation* result) {
    if (!result) return E_POINTER;
    try {
        const auto hr = validateSearchViewPresentation(actual); if (FAILED(hr)) return hr;
        auto candidate = actual;
        if (candidate.mode && *candidate.mode != SearchViewMode::Details &&
            *candidate.mode != SearchViewMode::Icons && *candidate.mode != SearchViewMode::Tiles) candidate.mode.reset();
        *result = std::move(candidate); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT search_scope_internal::requiresProtectiveGuard(const std::vector<SearchScopeRule>& rules, bool* required) {
    if (!required) return E_POINTER;
    try { bool candidate = false; const auto hr = protectiveGuardRequired(rules, candidate); if (SUCCEEDED(hr)) *required = candidate; return hr; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT search_scope_internal::createScopeGuard(const std::vector<SearchScopeRule>& rules, ICondition** result) {
    if (!result) return E_POINTER;
    try { ComPtr<ICondition> guard; const auto hr = scopeGuard(rules, &guard); return FAILED(hr) ? hr : guard.CopyTo(result); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT search_scope_internal::sameScopeGuard(ICondition* expected, ICondition* actual, bool* same) {
    if (!same) return E_POINTER;
    if (!expected || !actual) return E_INVALIDARG;
    try {
        std::wstring first, second; unsigned nodes = 0;
        auto hr = guardSignature(expected, first, nodes); if (FAILED(hr)) return hr;
        nodes = 0; hr = guardSignature(actual, second, nodes);
        if (SUCCEEDED(hr)) *same = first == second;
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT createSearchFolder(const std::wstring& query, IShellItem* scope, IShellItem** result, bool recursive) {
    if (!result) return E_POINTER;
    *result = nullptr;
    ComPtr<IShellItem> item;
    auto hr = validatedScope(scope, &item);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItemArray> scopes;
    hr = SHCreateShellItemArrayFromShellItem(item.Get(), IID_PPV_ARGS(&scopes));
    return FAILED(hr) ? hr : createSearchFolderForScopes(query, scopes.Get(), result, recursive);
}

HRESULT createSearchFolderForScopes(const std::wstring& query, IShellItemArray* scope,
                                    IShellItem** result, bool recursive) {
    if (!result) return E_POINTER;
    *result = nullptr;
    try {
        std::vector<SearchScopeRule> rules;
        const auto hr = rulesFromArray(scope, recursive, rules);
        return FAILED(hr) ? hr : createSearchFolderForScopeRules(query, rules, result);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT validateSearchDescriptorQuery(const std::wstring& query) {
    try {
        std::wstring text;auto hr=queryText(query,text);if(FAILED(hr))return hr;
        ComPtr<ICondition> condition;return parseQuery(text,false,&condition);
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
}

HRESULT searchScopeRulesRequireBacking(const std::vector<SearchScopeRule>& source, bool* required,
                                      std::vector<SearchScopeRule>* descriptorRules) {
    if (!required) return E_POINTER;
    try {
        std::vector<SearchScopeRule> physical;
        const auto physicalStatus = normalizeRules(source, physical, ScopeDomainValidation::PhysicalLive);
        if (physicalStatus == S_OK) { *required = false; return S_OK; }
        if (physicalStatus != unsupported) return FAILED(physicalStatus)?physicalStatus:E_UNEXPECTED;
        std::vector<SearchScopeRule> descriptor;
        const auto descriptorStatus = normalizeRules(source, descriptor, ScopeDomainValidation::NativeDescriptor);
        if (descriptorStatus != S_OK) return FAILED(descriptorStatus)?descriptorStatus:E_UNEXPECTED;
        const bool shallowInclude = std::any_of(descriptor.begin(), descriptor.end(), [](const auto& rule) {
            return !rule.excluded && !rule.recursive;
        });
        if (!shallowInclude) return physicalStatus;
        if (descriptorRules) *descriptorRules = std::move(descriptor);
        *required = true; return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT createSearchFolderForScopeRules(const std::wstring& query,
                                       const std::vector<SearchScopeRule>& scope, IShellItem** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    try {
        std::wstring text;
        auto hr = queryText(query, text);
        if (FAILED(hr)) return hr;

        ComPtr<ICondition> condition;
        hr = parseQuery(text, true, &condition);
        if (FAILED(hr)) return hr;

        ComPtr<IShellItemArray> scopes;
        std::vector<SearchScopeRule> rules;
        hr = normalizeRules(scope, rules, ScopeDomainValidation::PhysicalLive);
        if (SUCCEEDED(hr)) hr = includedArray(rules, &scopes);
        if (FAILED(hr)) return hr;
        {
            ComPtr<ICondition> restricted;
            hr = restrictToRules(condition.Get(), rules, &restricted);
            if (FAILED(hr)) return hr;
            condition = restricted;
        }

        ComPtr<ISearchFolderItemFactory> factory;
        hr = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        hr = factory->SetScope(scopes.Get());
        if (SUCCEEDED(hr)) hr = factory->SetCondition(condition.Get());
        if (SUCCEEDED(hr)) hr = factory->SetDisplayName(text.c_str());
        if (SUCCEEDED(hr)) hr = factory->SetFolderTypeID(FOLDERTYPEID_GenericSearchResults);
        if (SUCCEEDED(hr)) hr = factory->SetFolderLogicalViewMode(FLVM_DETAILS);
        return FAILED(hr) ? hr : factory->GetShellItem(IID_PPV_ARGS(result));
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT saveSearch(const std::wstring& query, IShellItem* scope, bool recursive,
                   const std::filesystem::path& path, SearchSaveMode mode,
                   const SearchViewPresentation* presentation, const SearchFileProperties* fileProperties) {
    ComPtr<IShellItem> item;
    auto hr = validatedScope(scope, &item);
    if (FAILED(hr)) return hr;
    ComPtr<IShellItemArray> scopes;
    hr = SHCreateShellItemArrayFromShellItem(item.Get(), IID_PPV_ARGS(&scopes));
    return FAILED(hr) ? hr : saveSearchForScopes(query, scopes.Get(), recursive, path, mode, presentation, fileProperties);
}

HRESULT saveSearchForScopes(const std::wstring& query, IShellItemArray* scopes, bool recursive,
                            const std::filesystem::path& path, SearchSaveMode mode,
                            const SearchViewPresentation* presentation, const SearchFileProperties* fileProperties) {
    try {
        std::vector<SearchScopeRule> rules;
        const auto hr = rulesFromArray(scopes, recursive, rules);
        return FAILED(hr) ? hr : saveSearchForScopeRules(query, rules, path, mode, presentation, fileProperties);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT saveSearchForScopeRules(const std::wstring& query, const std::vector<SearchScopeRule>& scopes,
                               const std::filesystem::path& path, SearchSaveMode mode,
                               const SearchViewPresentation* presentation, const SearchFileProperties* fileProperties,
                               SearchCreatedFileProof* createdFileProof) {
    if (createdFileProof) *createdFileProof = {};
    try {
        if (mode != SearchSaveMode::CreateNew && mode != SearchSaveMode::UserConfirmed) return E_INVALIDARG;
        if (createdFileProof && mode != SearchSaveMode::CreateNew) return E_INVALIDARG;
        if (path.empty() || path.native().find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        auto extension = path.extension().native();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(towlower(c));
        });
        if (extension != L".search-ms") return E_INVALIDARG;
        std::wstring text;
        auto hr = queryText(query, text);
        if (FAILED(hr)) return hr;
        std::vector<SearchScopeRule> searchScopes;
        hr = normalizeRules(scopes, searchScopes, ScopeDomainValidation::NativeDescriptor);
        if (FAILED(hr)) return hr;
        ComPtr<ICondition> condition;
        ComPtr<IQuerySolution> resolver;
        ComPtr<IQueryParser> parser;
        hr = parseQuery(text, false, &condition, &resolver, &parser);
        if (FAILED(hr)) return hr;
        bool guarded = false;
        if (FAILED(hr = protectiveGuardRequired(searchScopes, guarded))) return hr;
        if (guarded) {
            ComPtr<ICondition> guard;
            if (FAILED(hr = scopeGuard(searchScopes, &guard))) return hr;
            // Keep a recognizable two-child boundary around the original
            // condition. Never simplify or freeze its relative-date context.
            bool indistinguishable = false;
            if (FAILED(hr = search_scope_internal::sameScopeGuard(guard.Get(), condition.Get(), &indistinguishable))) return hr;
            if (indistinguishable) return unsupported;
            ComPtr<IConditionFactory2> factory;
            hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(hr)) return hr;
            ICondition* branches[]{condition.Get(), guard.Get()};
            ComPtr<ICondition> protectedQuery;
            hr = factory->CreateCompoundFromArray(CT_AND_CONDITION, branches, 2,
                CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(&protectedQuery));
            if (FAILED(hr)) return hr;
            condition = std::move(protectedQuery);
        }
        SearchViewPresentation defaultPresentation;
        defaultPresentation.mode = SearchViewMode::Details;
        defaultPresentation.iconSize = 16;
        const auto& display = presentation ? *presentation : defaultPresentation;
        if (FAILED(hr = validateSearchViewPresentation(display))) return hr;
        if (display.mode && *display.mode != SearchViewMode::Details &&
            *display.mode != SearchViewMode::Icons && *display.mode != SearchViewMode::Tiles) return unsupported;
        // Serialize only the public format; unsupported presentation fails
        // before opening the target, including a user-confirmed replacement.
        std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                           L"<persistedQuery version=\"1.0\">\r\n<viewInfo";
        if (display.mode) xml += L" viewMode=\"" + std::wstring(*display.mode == SearchViewMode::Details ? L"details" :
                                  *display.mode == SearchViewMode::Icons ? L"icons" : L"tiles") + L"\"";
        if (display.iconSize) xml += L" iconSize=\"" + std::to_wstring(*display.iconSize) + L"\"";
        xml += L">\r\n";
        if (display.visibleColumns) {
            xml += L"<visibleColumns>\r\n";
            for (const auto& column : *display.visibleColumns) {
                xml += L"<column";
                if (FAILED(hr = appendAttribute(xml, L"viewField", column))) return hr;
                xml += L"/>\r\n";
            }
            xml += L"</visibleColumns>\r\n";
        }
        const auto orderXml = [&](const wchar_t* tag, const SearchViewOrder& order) -> HRESULT {
            xml += L"<" + std::wstring(tag);
            const auto status = appendAttribute(xml, L"viewField", order.property);
            if (FAILED(status)) return status;
            xml += L" direction=\"" + std::wstring(
                order.direction == SORT_ASCENDING ? L"ascending" : L"descending") + L"\"/>\r\n";
            return S_OK;
        };
        if (display.groupBy && FAILED(hr = orderXml(L"groupBy", *display.groupBy))) return hr;
        if (display.sort) {
            xml += L"<sortList>\r\n";
            for (const auto& order : *display.sort) if (FAILED(hr = orderXml(L"sort", order))) return hr;
            xml += L"</sortList>\r\n";
        }
        xml += L"</viewInfo>\r\n<query>\r\n";
        hr = scopeXml(searchScopes, xml);
        if (FAILED(hr)) return hr;
        xml += L"<conditions>\r\n";
        unsigned conditionCount = 0;
        hr = conditionXml(condition.Get(), xml, 0, conditionCount, resolver.Get(), parser.Get());
        if (FAILED(hr)) return hr;
        // Windows 10's loader requires an explicit kind union even when the
        // query does not narrow file kinds. "item" includes every item kind.
        xml += L"</conditions>\r\n<kindList><kind name=\"item\"/></kindList>\r\n</query>\r\n";
        if (fileProperties && FAILED(hr = filePropertiesXml(*fileProperties, xml))) return hr;
        xml += L"</persistedQuery>\r\n";
        std::string bytes;
        hr = xmlBytes(xml, bytes);
        if (FAILED(hr)) return hr;
        // Generated files must respect the reader's resource limits, even
        // for large scopes and escaped property values. Unsupported native
        // semantic shapes still remain available through the native viewer.
        if (FAILED(hr = saved_search_internal::validateSerializedLimits(bytes))) return hr;
        bool replaced = false;
        hr = mode == SearchSaveMode::CreateNew ? writeNewFile(path, bytes, createdFileProof) : writeConfirmedFile(path, bytes, replaced);
        if (SUCCEEDED(hr)) {
            // A saved-search PIDL can cache its parsed query. Publish the
            // completed change after all file handles close, so a fresh native
            // open observes the replacement rather than the old condition.
            const auto absolute = std::filesystem::absolute(path);
            SHChangeNotify(replaced ? SHCNE_UPDATEITEM : SHCNE_CREATE,
                           SHCNF_PATHW | SHCNF_FLUSHNOWAIT, absolute.c_str(), nullptr);
        }
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::filesystem::filesystem_error& error) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(error.code().value()));
    }
}

} // namespace explorer
