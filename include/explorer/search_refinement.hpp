#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace explorer {
enum class SearchRefinementCategory : unsigned { Kind, Date, Size };

// Immutable native condition snapshot. Create/use/release on its owning STA.
// Inspection never changes query text; replacement is validated before output.
class NativeSearchRefinements final {
public:
    static HRESULT inspect(const std::wstring& query, std::shared_ptr<NativeSearchRefinements>* result) noexcept;
    // Native enum values are not AQS literals. Produce and validate every
    // expression outside Ribbon property callbacks; preserve output on failure.
    static HRESULT kindPresets(const std::vector<std::wstring>& values, std::vector<std::wstring>* result) noexcept;
    HRESULT replace(SearchRefinementCategory category, const std::wstring& preset, std::wstring* result) const noexcept;
    HRESULT matches(SearchRefinementCategory category, const std::wstring& preset, bool* result) const noexcept;
    ~NativeSearchRefinements();
private:
    struct Impl;
    explicit NativeSearchRefinements(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
}
