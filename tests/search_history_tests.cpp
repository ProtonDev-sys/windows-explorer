#include "explorer/search_history.hpp"
#include "explorer/headless_visual.hpp"
#include <wrl/client.h>
#include <shobjidl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <future>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        GUID id{}; wchar_t name[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, name, ARRAYSIZE(name)), "Create history fixture identity");
        root = std::filesystem::temp_directory_path() / (std::wstring(L"WindowsExplorer-SearchHistory-") + name);
        require(std::filesystem::create_directory(root), "Create owned history fixture");
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
void persistence() {
    Fixture fixture; const auto path = fixture.root / L"search-history.dat";
    const std::vector<std::wstring> queries{L"System.FileName:日本語😀", L"kind:picture AND date:today", L"tab\tline\nquery"};
    std::vector<std::wstring> loaded{L"unchanged"};
    require(FAILED(explorer::loadSearchHistory(path, &loaded)) && loaded[0] == L"unchanged", "Missing file changed output");
    require(SUCCEEDED(explorer::saveSearchHistory(path, queries)), "Persist Unicode MRU");
    require(SUCCEEDED(explorer::loadSearchHistory(path, &loaded)) && loaded == queries, "Restore exact Unicode MRU order");
    const std::vector<std::wstring> updated{L"replacement"};
    require(SUCCEEDED(explorer::saveSearchHistory(path, updated)) &&
        SUCCEEDED(explorer::loadSearchHistory(path, &loaded)) && loaded == updated, "Atomic MRU replacement");
    auto invalid = updated; invalid.push_back(std::wstring(1, static_cast<wchar_t>(0xd800)));
    require(FAILED(explorer::saveSearchHistory(path, invalid)) &&
        SUCCEEDED(explorer::loadSearchHistory(path, &loaded)) && loaded == updated, "Invalid Unicode replaced valid MRU");
    require(FAILED(explorer::saveSearchHistory(fixture.root, updated)), "Directory MRU target accepted");
    require(explorer::loadSearchHistory(path, nullptr) == E_POINTER &&
        explorer::saveSearchHistory(L"relative.dat", updated) == E_INVALIDARG, "History path/output guards");
    std::ofstream corrupt(path, std::ios::binary | std::ios::trunc); corrupt << "bad version"; corrupt.close();
    loaded = updated;
    require(FAILED(explorer::loadSearchHistory(path, &loaded)) && loaded == updated, "Malformed file changed output");
    require(std::distance(std::filesystem::directory_iterator(fixture.root), std::filesystem::directory_iterator{}) == 1,
        "Atomic history left staging files");
}
void ordering() {
    std::vector<std::wstring> queries;
    for (unsigned i = 0; i < 25; ++i) require(explorer::rememberSearch(queries, L"term " + std::to_wstring(i)), "Remember query");
    require(queries.size() == explorer::maximumRecentSearches && queries.front() == L"term 24" && queries.back() == L"term 5",
        "Bounded MRU order");
    require(explorer::rememberSearch(queries, L" term 12 ") && queries.front() == L"term 12" && queries.size() == 20,
        "Duplicate MRU did not move to front");
    const auto before = queries;
    require(!explorer::rememberSearch(queries, L"  ") && !explorer::rememberSearch(queries, std::wstring(L"a\0b", 3)) &&
        queries == before, "Invalid query changed MRU");
}
void enumeration() {
    ComPtr<explorer::SearchSuggestionList> source; source.Attach(new explorer::SearchSuggestionList);
    const std::vector<std::wstring> queries{L"first", L"日本語😀", L"third"};
    require(SUCCEEDED(source->replace(queries)), "Set native autocomplete source");
    LPOLESTR text = nullptr;
    require(source->Next(1, &text, nullptr) == S_OK && std::wstring(text) == L"first", "Native first suggestion");
    CoTaskMemFree(text);
    ComPtr<IEnumString> clone; require(source->Clone(&clone) == S_OK, "Clone native enumeration position");
    require(source->Skip(99) == S_FALSE && source->Next(1, &text, nullptr) == S_FALSE, "Bounded enumerator exhaustion");
    require(clone->Next(1, &text, nullptr) == S_OK && std::wstring(text) == L"日本語😀", "Independent clone Unicode position");
    CoTaskMemFree(text);
    require(source->Reset() == S_OK, "Reset suggestions");
    LPOLESTR batch[4]{}; ULONG fetched = 99;
    require(source->Next(4, batch, &fetched) == S_FALSE && fetched == 3, "Partial native batch count");
    for (ULONG i = 0; i < fetched; ++i) { require(std::wstring(batch[i]) == queries[i], "Native batch order"); CoTaskMemFree(batch[i]); }
    const std::vector<std::wstring> changed{L"new query"};
    require(source->replace(changed) == S_OK && source->Next(1, &text, nullptr) == S_OK && std::wstring(text) == changed[0],
        "Native list did not refresh its owned snapshot"); CoTaskMemFree(text);
    require(clone->Next(1, &text, nullptr) == S_OK && std::wstring(text) == L"third", "Replace mutated existing clone");
    CoTaskMemFree(text);
    require(source->Next(2, batch, nullptr) == E_POINTER && source->Clone(nullptr) == E_POINTER,
        "IEnumString native contract guards");
    IAutoComplete2* sentinel = reinterpret_cast<IAutoComplete2*>(static_cast<uintptr_t>(1));
    require(explorer::attachSearchSuggestions(nullptr, source.Get(), true, &sentinel) == E_ACCESSDENIED &&
        sentinel == reinterpret_cast<IAutoComplete2*>(static_cast<uintptr_t>(1)), "Headless autocomplete activated UI or changed output");
}
void nativeAutocomplete() {
    // Attach the actual Shell component on a new private-desktop STA, without
    // ever focusing/showing its edit or switching the user's input desktop.
    auto worker = std::async(std::launch::async, [] {
        explorer::PrivateDesktop desktop;
        require(SUCCEEDED(desktop.initialize()) && SUCCEEDED(desktop.verifyIsolation()), "Create autocomplete private desktop");
        require(SUCCEEDED(OleInitialize(nullptr)), "Initialize autocomplete STA");
        struct OleScope { ~OleScope() { OleUninitialize(); } } ole;
        const auto parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 220, 50, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(parent != nullptr, "Create hidden autocomplete host");
        struct WindowScope { HWND value; ~WindowScope() { DestroyWindow(value); } } host{parent};
        const auto edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE, 0, 0, 200, 30, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(edit != nullptr, "Create hidden owned search edit");
        WindowScope window{edit};
        ComPtr<explorer::SearchSuggestionList> source; source.Attach(new explorer::SearchSuggestionList);
        const std::vector<std::wstring> queries{L"owned suggestion", L"日本語😀"};
        require(source->replace(queries) == S_OK, "Initialize owned suggestions");
        ComPtr<IAutoComplete2> autocomplete;
        const auto status = explorer::attachSearchSuggestions(edit, source.Get(), false, &autocomplete);
        if (explorer::searchSuggestionsAllowed()) {
            require(SUCCEEDED(status) && autocomplete, "Attach actual native autocomplete");
            DWORD flags = 0;
            require(autocomplete->GetOptions(&flags) == S_OK &&
                flags == (ACO_AUTOSUGGEST | ACO_UPDOWNKEYDROPSLIST), "Native autosuggest option readback");
            ComPtr<IAutoCompleteDropDown> dropdown;
            require(SUCCEEDED(autocomplete.As(&dropdown)), "Native autocomplete dropdown interface");
            DWORD visible = 99; PWSTR selected = nullptr;
            require(SUCCEEDED(dropdown->GetDropDownStatus(&visible, &selected)) && !(visible & ACDD_VISIBLE),
                "Autocomplete showed its dropdown without input"); CoTaskMemFree(selected);
            require(SUCCEEDED(autocomplete->Enable(FALSE)), "Disable native autocomplete for teardown");
        } else require(status == HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY) && !autocomplete,
            "Native search policy was not respected");
        require(!IsWindowVisible(edit) && SUCCEEDED(desktop.verifyIsolation()), "Autocomplete exposed input-desktop UI");
    });
    worker.get();
}
}
int runSearchHistoryTests() {
    unsigned failures = 0;
    const std::pair<const char*, void(*)()> groups[]{{"owned Unicode persistence and atomic failure", persistence},
        {"bounded deduplicated recent order", ordering}, {"native IEnumString ownership, clone and headless guard", enumeration},
        {"actual Shell autocomplete attachment on private desktop", nativeAutocomplete}};
    for (const auto& [name, test] : groups) {
        try { test(); std::cout << "PASS: Search history: " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL: Search history: " << name << ": " << error.what() << '\n'; }
    }
    return static_cast<int>(failures);
}
