#include "explorer/search.hpp"
#include <shlobj.h>
#include <shlguid.h>
#include <structuredquery.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <array>

namespace explorer {
namespace {
using Microsoft::WRL::ComPtr;

HRESULT parseQuery(const std::wstring& text, ICondition** result) {
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

    // Relative dates and virtual properties need resolution before execution.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return solution->Resolve(parsed.Get(), SQRO_DONT_SPLIT_WORDS, &now, result);
}
} // namespace

HRESULT createSearchFolder(const std::wstring& query, IShellItem* scope, IShellItem** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    const auto first = query.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return E_INVALIDARG;
    const auto last = query.find_last_not_of(L" \t\r\n");
    const auto text = query.substr(first, last - first + 1);

    ComPtr<ICondition> condition;
    auto hr = parseQuery(text, &condition);
    if (FAILED(hr)) return hr;

    ComPtr<IShellItem> searchScope = scope;
    if (!searchScope) {
        hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&searchScope));
        if (FAILED(hr)) return hr;
    }
    SFGAOF attributes = 0;
    hr = searchScope->GetAttributes(SFGAO_FOLDER, &attributes);
    if (FAILED(hr)) return hr;
    if (!(attributes & SFGAO_FOLDER)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);

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

} // namespace explorer
