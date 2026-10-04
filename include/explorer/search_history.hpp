#pragma once
#include "explorer/core.hpp"
#include <shldisp.h>
#include <objidl.h>
#include <atomic>
#include <vector>
#include <span>
#include <mutex>

namespace explorer {
constexpr size_t maximumRecentSearches = 20;
bool searchSuggestionsAllowed();
std::filesystem::path searchHistoryPath();
HRESULT loadSearchHistory(const std::filesystem::path& path, std::vector<std::wstring>* result);
HRESULT saveSearchHistory(const std::filesystem::path& path, std::span<const std::wstring> queries);
bool rememberSearch(std::vector<std::wstring>& queries, const std::wstring& query);

// A native autocomplete source that owns its strings independently of the
// host edit control. Clone snapshots have independent enumeration positions.
class SearchSuggestionList final : public IEnumString {
public:
    HRESULT replace(std::span<const std::wstring> queries);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Next(ULONG count, LPOLESTR* values, ULONG* fetched) override;
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE Clone(IEnumString** result) override;
private:
    ~SearchSuggestionList() = default;
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::vector<std::wstring> queries_;
    size_t position_ = 0;
};
HRESULT attachSearchSuggestions(HWND edit, SearchSuggestionList* source, bool headless,
                                IAutoComplete2** result);
} // namespace explorer
