#pragma once

#include "explorer/core.hpp"
#include <optional>

namespace explorer {

enum class LiveSearchKind { Query, ReturnToOrigin };
struct LiveSearchRequest {
    std::uint64_t generation = 0;
    std::wstring literal;
    LiveSearchKind kind = LiveSearchKind::Query;
    bool explicitSubmit = false;
    bool operator==(const LiveSearchRequest&) const = default;
};

// Scheduling only: the host keeps the origin PIDL and executes the public
// native search factory. Ticks are monotonic milliseconds (GetTickCount64).
// There is one replaceable request, so typing while navigation is in progress
// cannot create an unbounded queue. Automatic requests never commit history.
class LiveSearchPolicy {
public:
    static constexpr std::uint32_t defaultDebounceMilliseconds = 250;
    static constexpr std::size_t maximumLiteralLength = 32768;
    explicit LiveSearchPolicy(std::uint32_t debounceMilliseconds = defaultDebounceMilliseconds);

    HRESULT userEdited(const std::wstring& literal, std::uint64_t now);
    HRESULT submit(const std::wstring& literal, std::uint64_t now);
    void escape(std::uint64_t now);
    // Use for EVERY programmatic edit control update. It invalidates pending
    // and issued work, and emits neither a query nor a history record.
    HRESULT replaceText(const std::wstring& literal);
    // External navigation / scope replacement / teardown invalidates work.
    void cancel();

    std::optional<LiveSearchRequest> takeReady(std::uint64_t now, bool navigationReady = true);
    bool current(const LiveSearchRequest& request) const;
    // Call for a factory failure or actual matching navigation completion,
    // not merely after BrowseToObject accepts a navigation. False means stale
    // or already finished; callers must not update UI/history for that result.
    bool finish(const LiveSearchRequest& request, HRESULT result);

    const std::wstring& literal() const { return literal_; }
    const std::wstring& committedLiteral() const { return committedLiteral_; }
    bool waiting() const { return pending_; }
    std::optional<std::uint64_t> deadline() const;

private:
    HRESULT schedule(const std::wstring& literal, std::uint64_t now, bool submit);
    void invalidate();
    std::uint32_t debounce_;
    std::uint64_t generation_ = 0;
    std::uint64_t deadline_ = 0;
    std::wstring literal_;
    std::wstring committedLiteral_;
    std::optional<LiveSearchRequest> request_;
    bool pending_ = false;
    bool issued_ = false;
};

} // namespace explorer
