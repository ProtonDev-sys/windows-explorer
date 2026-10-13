#pragma once
#include "explorer/search.hpp"
#include <memory>

namespace explorer {

// Plain creator-STA read lease. Current/history/call owners retain their
// lease independently of the bounded Store cache. Releasing a lease closes
// its handle; it never deletes the published descriptor.
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
    static constexpr size_t maximumResidentBackings = 128;
    // Empty uses verified FOLDERID_LocalAppData, with one fresh session root.
    // Published descriptors survive cache eviction, close and destruction.
    // No old session is adopted/swept and no lifetime query quota applies.
    // Tests may supply an exclusively owned parent.
    explicit SearchBackingStore(std::filesystem::path ownedParent = {});
    ~SearchBackingStore();
    SearchBackingStore(const SearchBackingStore&) = delete;
    SearchBackingStore& operator=(const SearchBackingStore&) = delete;
    HRESULT build(const std::wstring& query, const std::vector<SearchScopeRule>& rules,
                  SearchFolderBuild* result);
    // Cache-owned exact-key/lease records only (<=128). App/current/history
    // aliases can retain independent leases beyond this cache working set.
    size_t retainedCount() const noexcept;
    std::filesystem::path directory() const;
    // Caller retains the existing App native-stack/worker teardown gates.
    // This releases only Store cache/root handles; independent leases remain
    // valid and ALL created descriptor paths persist. It never establishes
    // last foreign native use and performs no file deletion or use_count test.
    // A native directory identity error remains an error, with no removal.
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
