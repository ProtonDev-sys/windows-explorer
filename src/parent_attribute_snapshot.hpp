#pragma once
#include "explorer/core.hpp"
#include <windows.h>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace explorer::detail {
// One per-item GetFileAttributesW dominated large-selection refreshes (about
// 20 microseconds per item, 10,000 items). A single bounded enumeration of the
// shared parent answers the same existence/directory question for ordinary
// names; every other name is answered by GetFileAttributesW.
//
// Both answers are instantaneous UI facts, never operation authorization: a
// sequential per-item loop also observes item N later than item 1, and a
// concurrent change can make either stale. Native menu/provider invocation
// remains authoritative.
class ParentAttributeSnapshot {
public:
    static constexpr DWORD minimumSelection = 64;
    explicit ParentAttributeSnapshot(DWORD selected) noexcept : selected_(selected) {}
    DWORD attributes(const std::wstring& path) {
        if (const auto known = lookup(path)) return *known;
        return GetFileAttributesW(path.c_str());
    }
    // Test observation: whether the most recent answer came from the snapshot.
    bool lastFromSnapshot() const noexcept { return lastFromSnapshot_; }

private:
    std::optional<DWORD> lookup(const std::wstring& path) {
        lastFromSnapshot_ = false;
        if (selected_ < minimumSelection || failed_) return std::nullopt;
        // Only ordinary drive-letter or UNC paths below MAX_PATH normalize to
        // the same entry in both APIs.
        if (path.size() >= MAX_PATH || path.size() < 4 || path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) return std::nullopt;
        const bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && path[2] == L'\\';
        if (!drive && !path.starts_with(L"\\\\")) return std::nullopt;
        const auto separator = path.find_last_of(L'\\');
        if (separator == std::wstring::npos || separator + 1 >= path.size()) return std::nullopt;
        const std::wstring name = path.substr(separator + 1);
        // Win32 strips trailing dots/spaces and maps reserved device names,
        // including COM/LPT superscript digits; such entries exist only
        // through extended paths and are not what an ordinary spelling opens.
        if (!validLeafName(name)) return std::nullopt;
        // Shortcut attributes gate residentShortcutExtension's own
        // reparse/offline/recall checks; read them from the item itself.
        const auto dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos &&
            CompareStringOrdinal(name.c_str() + dot, -1, L".lnk", -1, TRUE) == CSTR_EQUAL) return std::nullopt;
        const std::wstring_view parent = std::wstring_view(path).substr(0, separator + 1);
        if (!loaded_) {
            loaded_ = true;
            if (!load(std::wstring(parent))) { failed_ = true; return std::nullopt; }
        } else if (parent != parent_) return std::nullopt; // Multiple parents: per-item fallback.
        // Exact case: case-sensitive directories and differently cased
        // aliases are resolved by the per-item native call.
        const auto found = entries_.find(name);
        if (found == entries_.end()) return std::nullopt;
        // Placeholder, reparse and offline entries can be disguised during
        // enumeration; their actual attributes must come from the item itself.
        constexpr DWORD exact = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
            FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
        if (found->second & exact) return std::nullopt;
        lastFromSnapshot_ = true;
        return found->second;
    }
    bool load(std::wstring parent) {
        parent_ = parent;
        WIN32_FIND_DATAW entry{};
        const auto search = FindFirstFileExW((parent + L"*").c_str(), FindExInfoBasic, &entry,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (search == INVALID_HANDLE_VALUE) return false;
        struct Search { HANDLE value; ~Search() { FindClose(value); } } owned{search};
        // A huge parent with a small selection would cost more than the
        // per-item calls. An intentional budget stop leaves unread names to
        // the per-item fallback; an enumeration error discards everything.
        const size_t budget = std::max<size_t>(static_cast<size_t>(selected_) * 4, 4096);
        try {
            entries_.reserve(std::min<size_t>(budget, static_cast<size_t>(selected_) * 2));
            for (;;) {
                const std::wstring_view name(entry.cFileName);
                if (name != L"." && name != L"..") entries_.emplace(entry.cFileName, entry.dwFileAttributes);
                if (entries_.size() >= budget) return true;
                if (!FindNextFileW(search, &entry)) {
                    if (GetLastError() == ERROR_NO_MORE_FILES) return true;
                    entries_.clear();
                    return false;
                }
            }
        } catch (...) { entries_.clear(); return false; }
    }
    DWORD selected_;
    bool loaded_ = false, failed_ = false, lastFromSnapshot_ = false;
    std::wstring parent_;
    std::unordered_map<std::wstring, DWORD> entries_;
};
} // namespace explorer::detail
