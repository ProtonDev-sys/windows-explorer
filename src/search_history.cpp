#include "state_file.hpp"
#include "explorer/search_history.hpp"
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace explorer {
namespace {
constexpr size_t maximumQueryLength = 32767;
constexpr size_t maximumHistoryBytes = maximumRecentSearches * maximumQueryLength * 4 + 256;
constexpr char magic[] = "WXSearchHistory1\n";
bool valid(const std::wstring& query) {
    return !query.empty() && query.size() <= maximumQueryLength && query.find(L'\0') == std::wstring::npos &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, query.data(), static_cast<int>(query.size()),
                            nullptr, 0, nullptr, nullptr) > 0;
}
std::string encoded(const std::wstring& query) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, query.data(), static_cast<int>(query.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, query.data(), static_cast<int>(query.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
void append32(std::string& data, size_t value) {
    for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<char>((value >> shift) & 255));
}
bool read32(const std::string& data, size_t& offset, size_t& value) {
    if (offset > data.size() || data.size() - offset < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<size_t>(static_cast<unsigned char>(data[offset++])) << shift;
    return true;
}
HRESULT invalidData() { return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
}
bool searchSuggestionsAllowed() {
    if (SHRestricted(REST_NORECENTDOCSHISTORY)) return false;
    for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(hive, L"Software\\Policies\\Microsoft\\Windows\\Explorer",
            L"DisableSearchBoxSuggestions", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0)
            return false;
    }
    return true;
}
std::filesystem::path searchHistoryPath() {
    const auto settings = preferencesPath();
    return settings.empty() ? std::filesystem::path{} : settings.parent_path() / L"search-history.dat";
}
bool rememberSearch(std::vector<std::wstring>& queries, const std::wstring& query) {
    const auto value = trim(query);
    if (!valid(value)) return false;
    queries.erase(std::remove(queries.begin(), queries.end(), value), queries.end());
    queries.insert(queries.begin(), value);
    if (queries.size() > maximumRecentSearches) queries.resize(maximumRecentSearches);
    return true;
}
HRESULT loadSearchHistory(const std::filesystem::path& path, std::vector<std::wstring>* result) {
    if (!result) return E_POINTER;
    if (path.empty() || !path.is_absolute()) return E_INVALIDARG;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return HRESULT_FROM_WIN32(static_cast<DWORD>(error.value()));
    if (size > maximumHistoryBytes) return invalidData();
    std::ifstream input(path, std::ios::binary);
    if (!input) return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    std::string data(static_cast<size_t>(size) + 1, '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(input.gcount()));
    if (input.bad() || data.size() > size || data.size() < sizeof(magic) - 1 ||
        std::memcmp(data.data(), magic, sizeof(magic) - 1)) return invalidData();
    size_t offset = sizeof(magic) - 1, count = 0;
    if (!read32(data, offset, count) || count > maximumRecentSearches) return invalidData();
    std::vector<std::wstring> values;
    for (size_t index = 0; index < count; ++index) {
        size_t bytes = 0;
        if (!read32(data, offset, bytes) || !bytes || bytes > maximumQueryLength * 4 ||
            bytes > data.size() - offset) return invalidData();
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + offset,
                                                static_cast<int>(bytes), nullptr, 0);
        if (length <= 0 || static_cast<size_t>(length) > maximumQueryLength) return invalidData();
        std::wstring value(static_cast<size_t>(length), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + offset,
            static_cast<int>(bytes), value.data(), length) || !valid(value) || trim(value) != value ||
            std::find(values.begin(), values.end(), value) != values.end()) return invalidData();
        values.push_back(std::move(value)); offset += bytes;
    }
    if (offset != data.size()) return invalidData();
    *result = std::move(values);
    return S_OK;
}
HRESULT saveSearchHistory(const std::filesystem::path& path, std::span<const std::wstring> queries) {
    if (path.empty() || !path.is_absolute() || queries.size() > maximumRecentSearches) return E_INVALIDARG;
    std::string data(magic, sizeof(magic) - 1); append32(data, queries.size());
    for (size_t index = 0; index < queries.size(); ++index) {
        const auto& query = queries[index];
        if (!valid(query) || trim(query) != query ||
            std::find(queries.begin(), queries.begin() + index, query) != queries.begin() + index) return E_INVALIDARG;
        const auto bytes = encoded(query); append32(data, bytes.size()); data += bytes;
    }
    return writeStateFileAtomic(path, std::string_view(data));
}
HRESULT SearchSuggestionList::replace(std::span<const std::wstring> queries) {
    if (queries.size() > maximumRecentSearches) return E_INVALIDARG;
    for (const auto& query : queries) if (!valid(query)) return E_INVALIDARG;
    std::lock_guard lock(mutex_); queries_.assign(queries.begin(), queries.end()); position_ = 0;
    return S_OK;
}
HRESULT SearchSuggestionList::QueryInterface(REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IEnumString) return E_NOINTERFACE;
    *result = static_cast<IEnumString*>(this); AddRef(); return S_OK;
}
ULONG SearchSuggestionList::AddRef() { return ++references_; }
ULONG SearchSuggestionList::Release() { const auto count = --references_; if (!count) delete this; return count; }
HRESULT SearchSuggestionList::Next(ULONG count, LPOLESTR* values, ULONG* fetched) {
    if (fetched) *fetched = 0;
    if (!values || (!fetched && count != 1)) return E_POINTER;
    std::lock_guard lock(mutex_);
    const auto available = std::min(static_cast<size_t>(count), queries_.size() - position_);
    size_t index = 0;
    for (; index < available; ++index) {
        const auto& query = queries_[position_ + index];
        values[index] = static_cast<LPOLESTR>(CoTaskMemAlloc((query.size() + 1) * sizeof(wchar_t)));
        if (!values[index]) {
            for (size_t previous = 0; previous < index; ++previous) { CoTaskMemFree(values[previous]); values[previous] = nullptr; }
            return E_OUTOFMEMORY;
        }
        std::memcpy(values[index], query.c_str(), (query.size() + 1) * sizeof(wchar_t));
    }
    position_ += available;
    if (fetched) *fetched = static_cast<ULONG>(available);
    return available == count ? S_OK : S_FALSE;
}
HRESULT SearchSuggestionList::Skip(ULONG count) {
    std::lock_guard lock(mutex_); const auto skipped = std::min(static_cast<size_t>(count), queries_.size() - position_);
    position_ += skipped; return skipped == count ? S_OK : S_FALSE;
}
HRESULT SearchSuggestionList::Reset() { std::lock_guard lock(mutex_); position_ = 0; return S_OK; }
HRESULT SearchSuggestionList::Clone(IEnumString** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    auto copy = new(std::nothrow) SearchSuggestionList;
    if (!copy) return E_OUTOFMEMORY;
    try { std::lock_guard lock(mutex_); copy->queries_ = queries_; copy->position_ = position_; }
    catch (const std::bad_alloc&) { copy->Release(); return E_OUTOFMEMORY; }
    *result = copy; return S_OK;
}
HRESULT attachSearchSuggestions(HWND edit, SearchSuggestionList* source, bool headless, IAutoComplete2** result) {
    if (!result) return E_POINTER;
    if (headless) return E_ACCESSDENIED;
    if (!source || !IsWindow(edit)) return E_INVALIDARG;
    DWORD process = 0; const auto thread = GetWindowThreadProcessId(edit, &process);
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) return E_ACCESSDENIED;
    APTTYPE apartment; APTTYPEQUALIFIER qualifier;
    auto hr = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(hr)) return hr;
    if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    if (!searchSuggestionsAllowed()) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    Microsoft::WRL::ComPtr<IAutoComplete2> autocomplete;
    hr = CoCreateInstance(CLSID_AutoComplete, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&autocomplete));
    if (SUCCEEDED(hr)) hr = autocomplete->Init(edit, source, nullptr, nullptr);
    if (SUCCEEDED(hr)) hr = autocomplete->SetOptions(ACO_AUTOSUGGEST | ACO_UPDOWNKEYDROPSLIST);
    if (FAILED(hr)) { if (autocomplete) autocomplete->Enable(FALSE); return hr; }
    *result = autocomplete.Detach(); return S_OK;
}
} // namespace explorer
