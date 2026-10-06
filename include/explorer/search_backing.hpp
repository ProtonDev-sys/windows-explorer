#pragma once
#include "explorer/search.hpp"
#include <memory>

namespace explorer {

// Plain creator-STA lease. The owning store, not cache/history pruning, keeps
// every published descriptor read-leased until explicit final native teardown.
class SearchBackingLease final {
public:
    ~SearchBackingLease();
    const std::filesystem::path& path() const noexcept;
    const FILE_ID_INFO& identity() const noexcept;
private:
    struct Impl;
    explicit SearchBackingLease(std::unique_ptr<Impl> value) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class SearchBackingStore;
};

// Declaration order also releases the native item before its plain lease.
struct SearchFolderBuild {
    std::shared_ptr<const SearchBackingLease> backing;
    Microsoft::WRL::ComPtr<IShellItem> item;
};

class SearchBackingStore final {
public:
    static constexpr size_t maximumBackings = 128;
    // Empty uses GetTempPathW. Tests may supply an exclusively owned parent.
    explicit SearchBackingStore(std::filesystem::path ownedParent = {});
    ~SearchBackingStore();
    SearchBackingStore(const SearchBackingStore&) = delete;
    SearchBackingStore& operator=(const SearchBackingStore&) = delete;
    HRESULT build(const std::wstring& query, const std::vector<SearchScopeRule>& rules,
                  SearchFolderBuild* result);
    size_t retainedCount() const noexcept;
    std::filesystem::path directory() const;
    // Caller must first release all browser/view/history source interfaces,
    // drain original native workers and drop all returned backing leases.
    // Outstanding leases return BUSY. Any ownership/sharing/deletion failure
    // preserves the unremoved object and returns its actual error. Destruction
    // alone closes handles and NEVER removes files under an unknown provider.
    HRESULT closeAfterNativeTeardown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Source-only test seam: after the actual CreateNew writer/notification,
    // before acquiring the read lease. The fixture performs real owned native
    // replacements/reentry here; production leaves the observer unset.
    void setCreatedFileObserverForNativeTest(
        void (*observer)(const std::filesystem::path&, const FILE_ID_INFO&, void*), void* context) noexcept;
    friend struct SearchBackingOwnershipNativeFixture;
};

// No fallback on a factory error. Verified descriptor-needed rules use their
// exact native .search-ms item; all other rules invoke the unchanged factory.
HRESULT buildSearchFolder(const std::wstring& query, const std::vector<SearchScopeRule>& rules,
                          SearchBackingStore* store, SearchFolderBuild* result);

} // namespace explorer
