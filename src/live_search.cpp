#include "explorer/live_search.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace explorer {
namespace {
bool validLiteral(const std::wstring& literal) {
    if (literal.size() > LiveSearchPolicy::maximumLiteralLength || literal.find(L'\0') != std::wstring::npos)
        return false;
    return literal.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, literal.data(),
        static_cast<int>(literal.size()), nullptr, 0, nullptr, nullptr) != 0;
}
bool emptyQuery(const std::wstring& literal) {
    return std::all_of(literal.begin(), literal.end(), [](wchar_t value) { return std::iswspace(value) != 0; });
}
std::uint64_t after(std::uint64_t now, std::uint32_t delay) {
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    return now > maximum - delay ? maximum : now + delay;
}
}

LiveSearchPolicy::LiveSearchPolicy(std::uint32_t debounceMilliseconds) : debounce_(debounceMilliseconds) {}

void LiveSearchPolicy::invalidate() {
    ++generation_;
    // Zero is reserved for a request which has never been scheduled.
    if (!generation_) ++generation_;
    request_.reset();
    pending_ = false;
    issued_ = false;
}

HRESULT LiveSearchPolicy::schedule(const std::wstring& literal, std::uint64_t now, bool explicitSubmit) {
    if (!validLiteral(literal)) return E_INVALIDARG;
    if (!explicitSubmit && request_ && literal_ == literal) return S_FALSE;
    // Allocate before changing the generation, preserving state on allocation
    // failure. Literal spelling, Unicode and outer whitespace stay unchanged.
    const auto kind = emptyQuery(literal) ? LiveSearchKind::ReturnToOrigin : LiveSearchKind::Query;
    LiveSearchRequest next{0, literal, kind, explicitSubmit && kind == LiveSearchKind::Query};
    std::wstring nextLiteral(literal);
    invalidate();
    next.generation = generation_;
    literal_.swap(nextLiteral);
    request_ = std::move(next);
    pending_ = true;
    deadline_ = kind == LiveSearchKind::ReturnToOrigin || explicitSubmit ? now : after(now, debounce_);
    return S_OK;
}

HRESULT LiveSearchPolicy::userEdited(const std::wstring& literal, std::uint64_t now) {
    return schedule(literal, now, false);
}
HRESULT LiveSearchPolicy::submit(const std::wstring& literal, std::uint64_t now) {
    return schedule(literal, now, true);
}
void LiveSearchPolicy::escape(std::uint64_t now) {
    // Escape forces an origin request even when the edit is already empty.
    invalidate();
    literal_.clear();
    request_ = LiveSearchRequest{generation_, {}, LiveSearchKind::ReturnToOrigin, false};
    pending_ = true;
    deadline_ = now;
}
HRESULT LiveSearchPolicy::replaceText(const std::wstring& literal) {
    if (!validLiteral(literal)) return E_INVALIDARG;
    std::wstring replacement(literal);
    invalidate();
    literal_.swap(replacement);
    return S_OK;
}
void LiveSearchPolicy::cancel() { invalidate(); }

std::optional<LiveSearchRequest> LiveSearchPolicy::takeReady(std::uint64_t now, bool navigationReady) {
    if (!pending_ || !navigationReady || now < deadline_) return std::nullopt;
    auto result = request_;
    pending_ = false;
    issued_ = true;
    return result;
}
bool LiveSearchPolicy::current(const LiveSearchRequest& request) const {
    return request_ && *request_ == request;
}
bool LiveSearchPolicy::retry(const LiveSearchRequest& request, std::uint64_t now) {
    if (!issued_ || !current(request)) return false;
    issued_ = false;
    pending_ = true;
    deadline_ = after(now, 100);
    return true;
}
bool LiveSearchPolicy::finish(const LiveSearchRequest& request, HRESULT result) {
    if (!issued_ || !current(request)) return false;
    if (SUCCEEDED(result) && request.explicitSubmit && request.kind == LiveSearchKind::Query)
        committedLiteral_ = request.literal;
    issued_ = false;
    return true;
}
std::optional<std::uint64_t> LiveSearchPolicy::deadline() const {
    return pending_ ? std::optional<std::uint64_t>{deadline_} : std::nullopt;
}

} // namespace explorer
