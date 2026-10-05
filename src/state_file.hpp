#pragma once

#include <windows.h>
#include <filesystem>
#include <span>
#include <string_view>

namespace explorer {

// Caller-owned application state only. Encode/validate the complete codec
// before calling. Relative paths retain the existing preferences/QAT contract.
// Existing files keep exact DACL semantics, creation time, basic attributes and
// filesystem compression. Security is verified on the empty stage before bytes.
// read-only, directory, reparse and encrypted targets are refused. Failures
// preserve the original file and remove only the exclusively created stage.
HRESULT writeStateFileAtomic(const std::filesystem::path& path,
                             std::span<const BYTE> bytes) noexcept;
inline HRESULT writeStateFileAtomic(const std::filesystem::path& path,
                                    std::string_view bytes) noexcept {
    return writeStateFileAtomic(path,
        std::span<const BYTE>(reinterpret_cast<const BYTE*>(bytes.data()), bytes.size()));
}

} // namespace explorer
