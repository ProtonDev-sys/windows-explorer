#include "explorer/search_refinement.hpp"
#include <structuredquery.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <functional>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT unsupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
constexpr unsigned maximumNodes = 4096;
constexpr size_t maximumText = 32768, maximumSignature = 4 * 1024 * 1024;
struct Leaf {
    PWSTR property = nullptr, semantic = nullptr;
    CONDITION_OPERATION operation{};
    PROPVARIANT value{};
    ~Leaf() { CoTaskMemFree(property); CoTaskMemFree(semantic); PropVariantClear(&value); }
};
HRESULT readLeaf(ICondition* condition, Leaf& leaf) {
    auto hr = condition->GetComparisonInfo(&leaf.property, &leaf.operation, &leaf.value);
    return FAILED(hr) ? hr : condition->GetValueType(&leaf.semantic);
}
HRESULT textValid(const std::wstring& text) {
    if (text.size() > maximumText || text.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    if (!text.empty() && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)) return E_INVALIDARG;
    return S_OK;
}
HRESULT children(ICondition* condition, std::vector<ComPtr<ICondition>>& result) {
    CONDITION_TYPE type{};
    auto hr = condition->GetConditionType(&type); if (FAILED(hr)) return hr;
    if (type == CT_NOT_CONDITION) {
        ComPtr<ICondition> child;
        hr = condition->GetSubConditions(IID_PPV_ARGS(&child));
        if (SUCCEEDED(hr) && child) result.push_back(std::move(child));
        return FAILED(hr) ? hr : result.empty() ? E_UNEXPECTED : S_OK;
    }
    if (type != CT_AND_CONDITION && type != CT_OR_CONDITION) return E_INVALIDARG;
    ComPtr<IEnumUnknown> enumeration;
    hr = condition->GetSubConditions(IID_PPV_ARGS(&enumeration)); if (FAILED(hr)) return hr;
    if (!enumeration) return E_UNEXPECTED;
    for (;;) {
        ComPtr<IUnknown> unknown;
        hr = enumeration->Next(1, &unknown, nullptr); if (hr == S_FALSE) return S_OK;
        if (FAILED(hr)) return hr;
        if (!unknown || result.size() >= maximumNodes) return unsupported;
        ComPtr<ICondition> child;
        hr = unknown.As(&child); if (FAILED(hr)) return hr;
        result.push_back(std::move(child));
    }
}
void field(std::wstring& target, std::wstring_view value) {
    target += std::to_wstring(value.size()) + L":"; target += value;
}
HRESULT leafSignature(ICondition* condition, std::wstring& result) {
    Leaf leaf; auto hr = readLeaf(condition, leaf); if (FAILED(hr)) return hr;
    std::wstring property = leaf.property ? leaf.property : L"";
    std::transform(property.begin(), property.end(), property.begin(), [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    field(result, property); result += leaf.semantic ? L"1:" : L"0:";
    field(result, leaf.semantic ? leaf.semantic : L"");
    result += std::to_wstring(leaf.operation) + L":" + std::to_wstring(leaf.value.vt) + L":";
    SERIALIZEDPROPERTYVALUE* bytes = nullptr; ULONG count = 0;
    hr = StgSerializePropVariant(&leaf.value, &bytes, &count);
    const std::unique_ptr<SERIALIZEDPROPERTYVALUE, decltype(&CoTaskMemFree)> storage(bytes, &CoTaskMemFree);
    if (FAILED(hr)) return hr;
    if ((!bytes && count) || count > maximumSignature / 2) return unsupported;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    const auto data = reinterpret_cast<const BYTE*>(bytes);
    for (ULONG index = 0; index < count; ++index) { result += hex[data[index] >> 4]; result += hex[data[index] & 15]; }
    return S_OK;
}
// Full native leaf fields; only associative/idempotent AND/OR normalization.
HRESULT signature(ICondition* condition, std::wstring& result, unsigned& count, unsigned depth = 0) {
    if (!condition || depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{}; auto hr = condition->GetConditionType(&type); if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        result = L"3{"; hr = leafSignature(condition, result); result += L"}"; return hr;
    }
    std::vector<std::wstring> parts;
    std::function<HRESULT(ICondition*, unsigned)> collect = [&](ICondition* node, unsigned level) -> HRESULT {
        if (level > 64 || ++count > maximumNodes) return unsupported;
        CONDITION_TYPE childType{}; auto status = node->GetConditionType(&childType); if (FAILED(status)) return status;
        if (type != CT_NOT_CONDITION && childType == type) {
            std::vector<ComPtr<ICondition>> nested;
            status = children(node, nested); if (FAILED(status)) return status;
            for (const auto& child : nested) { status = collect(child.Get(), level + 1); if (FAILED(status)) return status; }
            return S_OK;
        }
        std::wstring part; status = signature(node, part, count, level);
        if (SUCCEEDED(status)) parts.push_back(std::move(part)); return status;
    };
    std::vector<ComPtr<ICondition>> immediate;
    hr = children(condition, immediate); if (FAILED(hr)) return hr;
    for (const auto& child : immediate) { hr = collect(child.Get(), depth + 1); if (FAILED(hr)) return hr; }
    if (type != CT_NOT_CONDITION) {
        std::sort(parts.begin(), parts.end()); parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
        if (parts.size() == 1) { result = std::move(parts.front()); return S_OK; }
    }
    result = std::to_wstring(type) + L"{";
    for (const auto& part : parts) { field(result, part); if (result.size() > maximumSignature) return unsupported; }
    result += L"}"; return S_OK;
}
HRESULT categoryMask(ICondition* condition, unsigned& mask, unsigned& count, unsigned depth = 0) {
    if (!condition || depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{}; auto hr = condition->GetConditionType(&type); if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        Leaf leaf; hr = readLeaf(condition, leaf); if (FAILED(hr)) return hr;
        constexpr std::array properties{L"System.Kind", L"System.DateModified", L"System.Size"};
        for (unsigned index = 0; index < properties.size(); ++index)
            if (leaf.property && _wcsicmp(leaf.property, properties[index]) == 0) { mask |= 1u << index; return S_OK; }
        mask |= 8; return S_OK;
    }
    std::vector<ComPtr<ICondition>> subs; hr = children(condition, subs); if (FAILED(hr)) return hr;
    for (const auto& child : subs) { hr = categoryMask(child.Get(), mask, count, depth + 1); if (FAILED(hr)) return hr; }
    return S_OK;
}
HRESULT dateTokens(ICondition* condition, std::vector<std::wstring>& result, unsigned& count, unsigned depth = 0) {
    if (!condition || depth > 64 || ++count > maximumNodes) return unsupported;
    CONDITION_TYPE type{}; auto hr = condition->GetConditionType(&type); if (FAILED(hr)) return hr;
    if (type == CT_LEAF_CONDITION) {
        Leaf leaf; hr = readLeaf(condition, leaf); if (FAILED(hr)) return hr;
        if (!leaf.semantic || wcscmp(leaf.semantic, L"System.StructuredQueryType.DateTime") != 0) return S_OK;
        const auto add = [&](const wchar_t* value) -> HRESULT {
            if (!value || wcslen(value) > maximumText) return unsupported;
            std::wstring token; field(token, leaf.property ? leaf.property : L""); field(token, value);
            result.push_back(std::move(token)); return S_OK;
        };
        // Absolute DateTime values are already represented by FILETIME;
        // their complete typed value remains in the resolved leaf signature.
        if (leaf.value.vt == VT_FILETIME) return S_OK;
        if (leaf.value.vt == VT_LPWSTR) return add(leaf.value.pwszVal);
        if (leaf.value.vt == (VT_VECTOR | VT_LPWSTR)) {
            if (leaf.value.calpwstr.cElems > maximumNodes || (!leaf.value.calpwstr.pElems && leaf.value.calpwstr.cElems)) return unsupported;
            for (ULONG index = 0; index < leaf.value.calpwstr.cElems; ++index) { hr = add(leaf.value.calpwstr.pElems[index]); if (FAILED(hr)) return hr; }
            return S_OK;
        }
        return unsupported;
    }
    std::vector<ComPtr<ICondition>> subs; hr = children(condition, subs); if (FAILED(hr)) return hr;
    for (const auto& child : subs) { hr = dateTokens(child.Get(), result, count, depth + 1); if (FAILED(hr)) return hr; }
    return S_OK;
}
HRESULT compound(IConditionFactory2* factory, const std::vector<ComPtr<ICondition>>& nodes, ICondition** result) {
    if (nodes.size() == 1) return nodes.front().CopyTo(result);
    std::vector<ICondition*> raw; for (const auto& item : nodes) raw.push_back(item.Get());
    return factory->CreateCompoundFromArray(CT_AND_CONDITION, raw.data(), static_cast<ULONG>(raw.size()),
                                            CONDITION_CREATION_DEFAULT, IID_PPV_ARGS(result));
}
}
struct NativeSearchRefinements::Impl {
    DWORD owner = GetCurrentThreadId();
    std::wstring original;
    ComPtr<IQueryParser> parser;
    ComPtr<IQuerySolution> solution;
    ComPtr<IConditionFactory2> factory;
    ComPtr<ICondition> tree;
    struct Factor { ComPtr<ICondition> tree; unsigned mask = 0; };
    std::vector<Factor> factors;
    unsigned entangled = 0;
    HRESULT parse(const std::wstring& text, ComPtr<IQuerySolution>& context, ComPtr<ICondition>& condition) const {
        auto hr = textValid(text); if (FAILED(hr)) return hr;
        hr = parser->Parse(text.c_str(), nullptr, &context); if (FAILED(hr)) return hr;
        return context->GetQuery(&condition, nullptr);
    }
    HRESULT equivalent(ICondition* expected, IConditionFactory* expectedContext, ICondition* actual,
                       IConditionFactory* actualContext, bool& same) const {
        SYSTEMTIME now{}; GetLocalTime(&now);
        ComPtr<ICondition> expectedResolved, actualResolved;
        auto hr = expectedContext->Resolve(expected, SQRO_DONT_SPLIT_WORDS, &now, &expectedResolved); if (FAILED(hr)) return hr;
        hr = actualContext->Resolve(actual, SQRO_DONT_SPLIT_WORDS, &now, &actualResolved); if (FAILED(hr)) return hr;
        std::wstring first, second; unsigned count = 0;
        hr = signature(expectedResolved.Get(), first, count); if (FAILED(hr)) return hr;
        count = 0; hr = signature(actualResolved.Get(), second, count); if (FAILED(hr)) return hr;
        std::vector<std::wstring> firstDates, secondDates; count = 0;
        hr = dateTokens(expected, firstDates, count); if (FAILED(hr)) return hr;
        count = 0; hr = dateTokens(actual, secondDates, count); if (FAILED(hr)) return hr;
        std::sort(firstDates.begin(), firstDates.end()); std::sort(secondDates.begin(), secondDates.end());
        same = first == second && firstDates == secondDates; return S_OK;
    }
    HRESULT preset(SearchRefinementCategory category, const std::wstring& text, ComPtr<IQuerySolution>& context,
                   ComPtr<ICondition>& condition) const {
        const auto index = static_cast<unsigned>(category); if (index >= 3 || text.empty()) return E_INVALIDARG;
        auto hr = parse(text, context, condition); if (FAILED(hr)) return hr;
        unsigned mask = 0, count = 0; hr = categoryMask(condition.Get(), mask, count);
        return FAILED(hr) ? hr : mask == (1u << index) ? S_OK : E_INVALIDARG;
    }
};
NativeSearchRefinements::NativeSearchRefinements(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
NativeSearchRefinements::~NativeSearchRefinements() = default;
HRESULT NativeSearchRefinements::kindPresets(const std::vector<std::wstring>& values, std::vector<std::wstring>* result) noexcept {
    if (!result) return E_POINTER;
    try {
        std::shared_ptr<NativeSearchRefinements> snapshot;
        auto hr = inspect(L"", &snapshot); if (FAILED(hr)) return hr;
        const auto& state = *snapshot->implementation_;
        std::vector<std::wstring> expressions;
        for (const auto& canonical : values) {
            hr = textValid(canonical); if (FAILED(hr) || canonical.empty()) return E_INVALIDARG;
            Leaf value; hr = InitPropVariantFromString(canonical.c_str(), &value.value); if (FAILED(hr)) return hr;
            ComPtr<ICondition> leaf;
            hr = state.factory->MakeLeaf(L"System.Kind", COP_EQUAL, L"System.StructuredQueryType.String", &value.value,
                                         nullptr, nullptr, nullptr, FALSE, &leaf); if (FAILED(hr)) return hr;
            PWSTR raw = nullptr; hr = state.parser->RestateToString(leaf.Get(), FALSE, &raw);
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> restated(raw, &CoTaskMemFree);
            const auto validate = [&](const std::wstring& expression) -> HRESULT {
                ComPtr<IQuerySolution> context; ComPtr<ICondition> parsed;
                auto status = state.preset(SearchRefinementCategory::Kind, expression, context, parsed); if (FAILED(status)) return status;
                bool same = false;
                status = state.equivalent(leaf.Get(), context.Get(), parsed.Get(), context.Get(), same);
                return FAILED(status) ? status : same ? S_OK : unsupported;
            };
            std::wstring expression;
            if (SUCCEEDED(hr) && raw && *raw) { expression = raw; hr = validate(expression); }
            else if (SUCCEEDED(hr)) hr = unsupported;
            if (FAILED(hr)) {
                if (hr == E_OUTOFMEMORY) return hr;
                // System.Kind's documented enum names have the same letters
                // as their native values (AQS is case insensitive). Restatement
                // can lose an enum such as SearchFolder. Try the documented
                // canonical marker only for a bounded native identifier, then
                // require every resolved field to match the installed value.
                // https://learn.microsoft.com/en-us/windows/win32/properties/props-system-kind
                // https://learn.microsoft.com/en-us/windows/win32/search/-search-3x-advancedquerysyntax
                const bool identifier = canonical.size() <= 256 && std::all_of(canonical.begin(), canonical.end(), [](wchar_t codeUnit) {
                    return (codeUnit >= L'a' && codeUnit <= L'z') || (codeUnit >= L'A' && codeUnit <= L'Z') ||
                        (codeUnit >= L'0' && codeUnit <= L'9') || codeUnit == L'_';
                });
                if (!identifier) return hr;
                expression = L"System.Kind:=System.Kind#" + canonical;
                hr = validate(expression); if (FAILED(hr)) return hr;
            }
            expressions.push_back(std::move(expression));
        }
        *result = std::move(expressions); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_UNEXPECTED; }
}
HRESULT NativeSearchRefinements::inspect(const std::wstring& query, std::shared_ptr<NativeSearchRefinements>* result) noexcept {
    if (!result) return E_POINTER;
    try {
        auto state = std::make_unique<Impl>(); auto hr = textValid(query); if (FAILED(hr)) return hr;
        state->original = query;
        ComPtr<IQueryParserManager> manager;
        hr = CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)); if (FAILED(hr)) return hr;
        hr = manager->CreateLoadedParser(L"SystemIndex", GetUserDefaultUILanguage(), IID_PPV_ARGS(&state->parser)); if (FAILED(hr)) return hr;
        hr = manager->InitializeOptions(FALSE, TRUE, state->parser.Get()); if (FAILED(hr)) return hr;
        constexpr std::array<std::pair<const wchar_t*, const wchar_t*>, 5> defaults{{
            {L"System.StructuredQueryType.String", L"System.Generic.String"}, {L"System.StructuredQueryType.Integer", L"System.Generic.Integer"},
            {L"System.StructuredQueryType.DateTime", L"System.Generic.DateTime"}, {L"System.StructuredQueryType.Boolean", L"System.Generic.Boolean"},
            {L"System.StructuredQueryType.FloatingPoint", L"System.Generic.FloatingPoint"}}};
        for (const auto& [type, property] : defaults) {
            PROPVARIANT value{}; hr = InitPropVariantFromString(property, &value);
            if (SUCCEEDED(hr)) hr = state->parser->SetMultiOption(SQMO_DEFAULT_PROPERTY, type, &value);
            PropVariantClear(&value); if (FAILED(hr)) return hr;
        }
        hr = CoCreateInstance(__uuidof(ConditionFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&state->factory)); if (FAILED(hr)) return hr;
        // An empty initial query has no factors. The native factory does not
        // accept a zero-child compound; a selected preset supplies its own tree.
        if (!query.empty()) {
            hr = state->parse(query, state->solution, state->tree);
            if (FAILED(hr) || !state->tree) return FAILED(hr) ? hr : E_UNEXPECTED;
        }
        unsigned visited = 0;
        std::function<HRESULT(ICondition*, unsigned)> split = [&](ICondition* condition, unsigned depth) -> HRESULT {
            if (depth > 64 || ++visited > maximumNodes) return unsupported;
            CONDITION_TYPE type{}; auto status = condition->GetConditionType(&type); if (FAILED(status)) return status;
            if (type == CT_AND_CONDITION) {
                std::vector<ComPtr<ICondition>> subs; status = children(condition, subs); if (FAILED(status)) return status;
                for (const auto& child : subs) { status = split(child.Get(), depth + 1); if (FAILED(status)) return status; }
                return S_OK;
            }
            unsigned mask = 0, count = 0; status = categoryMask(condition, mask, count); if (FAILED(status)) return status;
            if (mask != 1 && mask != 2 && mask != 4) state->entangled |= mask & 7;
            state->factors.push_back({condition, mask}); return S_OK;
        };
        if (state->tree) { hr = split(state->tree.Get(), 0); if (FAILED(hr)) return hr; }
        auto snapshot = std::shared_ptr<NativeSearchRefinements>(new NativeSearchRefinements(std::move(state)));
        *result = std::move(snapshot); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_UNEXPECTED; }
}
HRESULT NativeSearchRefinements::matches(SearchRefinementCategory category, const std::wstring& presetText, bool* result) const noexcept {
    if (!result) return E_POINTER;
    try {
        const auto& state = *implementation_; if (GetCurrentThreadId() != state.owner) return RPC_E_WRONG_THREAD;
        const auto index = static_cast<unsigned>(category); if (index >= 3) return E_INVALIDARG;
        if (state.entangled & (1u << index)) { *result = false; return S_OK; }
        std::vector<ComPtr<ICondition>> factors;
        for (const auto& factor : state.factors) if (factor.mask == (1u << index)) factors.push_back(factor.tree);
        if (factors.empty()) { *result = false; return S_OK; }
        ComPtr<IQuerySolution> context; ComPtr<ICondition> preset, existing;
        auto hr = state.preset(category, presetText, context, preset); if (FAILED(hr)) return hr;
        hr = compound(state.factory.Get(), factors, &existing); if (FAILED(hr)) return hr;
        bool same = false; hr = state.equivalent(existing.Get(), state.solution.Get(), preset.Get(), context.Get(), same);
        if (SUCCEEDED(hr)) *result = same; return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_UNEXPECTED; }
}
HRESULT NativeSearchRefinements::replace(SearchRefinementCategory category, const std::wstring& presetText, std::wstring* result) const noexcept {
    if (!result) return E_POINTER;
    try {
        const auto& state = *implementation_; if (GetCurrentThreadId() != state.owner) return RPC_E_WRONG_THREAD;
        const auto index = static_cast<unsigned>(category); if (index >= 3) return E_INVALIDARG;
        if (state.entangled & (1u << index)) return unsupported;
        ComPtr<IQuerySolution> context; ComPtr<ICondition> preset;
        auto hr = state.preset(category, presetText, context, preset); if (FAILED(hr)) return hr;
        bool unchanged = false; hr = matches(category, presetText, &unchanged); if (FAILED(hr)) return hr;
        if (unchanged) { *result = state.original; return S_OK; }
        std::vector<ComPtr<ICondition>> retained;
        for (const auto& factor : state.factors) if (factor.mask != (1u << index)) retained.push_back(factor.tree);
        std::wstring text;
        if (!retained.empty()) {
            ComPtr<ICondition> base; hr = compound(state.factory.Get(), retained, &base); if (FAILED(hr)) return hr;
            PWSTR raw = nullptr; hr = state.parser->RestateToString(base.Get(), FALSE, &raw);
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> restated(raw, &CoTaskMemFree);
            if (FAILED(hr)) return hr;
            if (!raw) return E_UNEXPECTED;
            text = L"(" + std::wstring(raw) + L") AND ";
        }
        // Retain the chosen language-independent preset literally. The base
        // comes only from native subtree restatement, never text slicing.
        text += L"(" + presetText + L")";
        retained.push_back(preset);
        ComPtr<ICondition> candidate; hr = compound(state.factory.Get(), retained, &candidate); if (FAILED(hr)) return hr;
        ComPtr<IQuerySolution> reparsedContext; ComPtr<ICondition> reparsed;
        hr = state.parse(text, reparsedContext, reparsed); if (FAILED(hr)) return hr;
        // Original factors and a newly parsed preset have different lexical
        // contexts. Resolve each with its actual owner at one reference time.
        SYSTEMTIME now{}; GetLocalTime(&now);
        std::vector<ComPtr<ICondition>> resolvedFactors;
        for (const auto& factor : state.factors) if (factor.mask != (1u << index)) {
            ComPtr<ICondition> resolved;
            hr = state.solution->Resolve(factor.tree.Get(), SQRO_DONT_SPLIT_WORDS, &now, &resolved);
            if (FAILED(hr)) return hr;
            resolvedFactors.push_back(std::move(resolved));
        }
        ComPtr<ICondition> resolvedPreset, expectedResolved, actualResolved;
        hr = context->Resolve(preset.Get(), SQRO_DONT_SPLIT_WORDS, &now, &resolvedPreset); if (FAILED(hr)) return hr;
        resolvedFactors.push_back(std::move(resolvedPreset));
        hr = compound(state.factory.Get(), resolvedFactors, &expectedResolved); if (FAILED(hr)) return hr;
        hr = reparsedContext->Resolve(reparsed.Get(), SQRO_DONT_SPLIT_WORDS, &now, &actualResolved); if (FAILED(hr)) return hr;
        std::wstring expectedSignature, actualSignature; unsigned count = 0;
        hr = signature(expectedResolved.Get(), expectedSignature, count); if (FAILED(hr)) return hr;
        count = 0; hr = signature(actualResolved.Get(), actualSignature, count); if (FAILED(hr)) return hr;
        std::vector<std::wstring> expectedDates, actualDates; count = 0;
        hr = dateTokens(candidate.Get(), expectedDates, count); if (FAILED(hr)) return hr;
        count = 0; hr = dateTokens(reparsed.Get(), actualDates, count); if (FAILED(hr)) return hr;
        std::sort(expectedDates.begin(), expectedDates.end()); std::sort(actualDates.begin(), actualDates.end());
        if (expectedSignature != actualSignature || expectedDates != actualDates) return unsupported;
        *result = std::move(text); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_UNEXPECTED; }
}
}
