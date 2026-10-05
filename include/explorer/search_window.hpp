#pragma once

#include "explorer/search.hpp"
#include <span>

namespace explorer {

// This is the host's original editable context, not an interpretation of the
// native search folder's private PIDL or its desktop parsing name.
struct SearchWindowContext {
    std::wstring query;
    Microsoft::WRL::ComPtr<IShellItem> primaryScope;
    Microsoft::WRL::ComPtr<IShellItem> closeOrigin;
    Microsoft::WRL::ComPtr<IShellItemArray> scopes;
    std::vector<SearchScopeRule> rules;
    bool recursive = true;
    std::optional<SearchViewPresentation> presentation;
    std::optional<SearchFileProperties> fileProperties;
};

// Failed serialization/validation leaves the caller's output intact. Public
// scope PIDLs are bounded and walked before they are passed back to the Shell.
HRESULT encodeSearchWindowContext(const SearchWindowContext& context, std::vector<BYTE>* result);
HRESULT decodeSearchWindowContext(std::span<const BYTE> packet, SearchWindowContext* result);

class SearchWindowMapping {
public:
    SearchWindowMapping() = default;
    SearchWindowMapping(const SearchWindowMapping&) = delete;
    SearchWindowMapping& operator=(const SearchWindowMapping&) = delete;
    SearchWindowMapping(SearchWindowMapping&& other) noexcept;
    SearchWindowMapping& operator=(SearchWindowMapping&& other) noexcept;
    ~SearchWindowMapping();
    static HRESULT create(const SearchWindowContext& context, SearchWindowMapping* result);
    HRESULT read(SearchWindowContext* result) const;
    HANDLE handle() const noexcept { return handle_; }
private:
    HANDLE handle_ = nullptr;
};

// Accept ownership only after a genuine inherited read-only mapping and its
// complete packet have been validated. Failed calls never close an arbitrary
// handle supplied on the command line or change the output context.
HRESULT consumeSearchWindowContext(ULONG_PTR handleValue, SearchWindowContext* result);

// The App rejects headless NewWindow before calling this normal launch route.
// Exactly one reduced read-only mapping handle is inherited by the child.
// An optional process output is initialized to nullptr; after success the
// caller owns that handle and must close it. The ordinary App needs no handle.
HRESULT launchSearchWindow(const std::wstring& executable, const SearchWindowContext& context,
                           HANDLE* processHandle = nullptr);

} // namespace explorer
